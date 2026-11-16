#include "device.hpp"

#include "core/core.hpp"
#include "dinput/device_info.hpp"
#include "dinput/device_input.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/force_feedback/effect.hpp"
#include "dinput/force_feedback/parameters.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/profile.hpp"
#include "factory.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace inputnext::dinput {
namespace {

template <typename Operation>
HRESULT guarded(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return DIERR_OUTOFMEMORY;
    } catch (...) {
        return DIERR_GENERIC;
    }
}

class callback_reference final {
public:
    explicit callback_reference(IUnknown& object) noexcept : object_(object) { object_.AddRef(); }

    ~callback_reference() { object_.Release(); }

    callback_reference(const callback_reference&) = delete;
    callback_reference& operator=(const callback_reference&) = delete;
    callback_reference(callback_reference&&) = delete;
    callback_reference& operator=(callback_reference&&) = delete;

private:
    IUnknown& object_;
};

class device_lifetime final {
public:
    explicit device_lifetime(IUnknown& parent) noexcept : parent_(parent) {
        parent_.AddRef();
        object_created();
    }

    ~device_lifetime() {
        object_destroyed();
        parent_.Release();
    }

    device_lifetime(const device_lifetime&) = delete;
    device_lifetime& operator=(const device_lifetime&) = delete;
    device_lifetime(device_lifetime&&) = delete;
    device_lifetime& operator=(device_lifetime&&) = delete;

private:
    IUnknown& parent_;
};

[[nodiscard]] const GUID* property_token(const GUID* property) noexcept {
    // The COM ABI passes MAKEDIPROP values as pointer tokens. Preserve their bits
    // before comparing them, since C++ REFGUID implies alignment to the optimizer.
    const GUID* volatile token = property;
    return token;
}

template <typename Legacy, typename Info>
[[nodiscard]] DWORD requested_info_size(const Info* output) noexcept {
    DWORD size{};
    if (output != nullptr) {
        // The caller can provide only the legacy prefix through the COM signature.
        std::memcpy(&size, output, sizeof(size));
    }
    return size == sizeof(Info) || size == sizeof(Legacy) ? size : 0U;
}

template <typename Info>
void write_info(Info* output, Info value, DWORD size) noexcept {
    value.dwSize = size;
    std::memcpy(output, &value, size);
}

[[nodiscard]] bool valid_effect_filter(DWORD flags) noexcept {
    constexpr DWORD type_mask = 0xffU;
    constexpr DWORD attributes = DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_SATURATION |
                                 DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION |
                                 DIEFT_DEADBAND | DIEFT_STARTDELAY;
    const auto type = flags & type_mask;
    return (flags & ~(type_mask | attributes)) == 0U &&
           (type <= DIEFT_CUSTOMFORCE || type == DIEFT_HARDWARE);
}

[[nodiscard]] bool matches_effect_filter(const force_feedback::effect_descriptor& descriptor,
    DWORD flags) noexcept {
    const DWORD requested_type = flags & 0xffU;
    const DWORD effect_type = descriptor.type & 0xffU;
    if (requested_type != DIEFT_ALL && requested_type != DIEFT_HARDWARE &&
        requested_type != effect_type) {
        return false;
    }
    if (requested_type == DIEFT_HARDWARE) {
        return false;
    }
    constexpr DWORD attributes = DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_SATURATION |
                                 DIEFT_POSNEGCOEFFICIENTS | DIEFT_POSNEGSATURATION |
                                 DIEFT_DEADBAND | DIEFT_STARTDELAY;
    const DWORD requested_attributes = flags & attributes;
    return (descriptor.type & requested_attributes) == requested_attributes;
}

[[nodiscard]] bool is_feedback_actuator(const object_descriptor& object) noexcept {
    return object.kind == object_kind::axis && force_feedback::actuator(object.source_index);
}

template <typename Projection>
[[nodiscard]] auto describe_for_device(const object_descriptor& object,
    Projection projection,
    bool feedback) noexcept {
    auto result = projection(object);
    if (feedback && is_feedback_actuator(object)) {
        result.dwType |= DIDFT_FFACTUATOR;
        result.dwFlags |= DIDOI_FFACTUATOR;
    }
    return result;
}

[[nodiscard]] bool
matches_device_object(const object_descriptor& object, DWORD flags, bool feedback) noexcept {
    const bool requires_actuator = (flags & DIDFT_FFACTUATOR) != 0U;
    if (requires_actuator && (!feedback || !is_feedback_actuator(object))) {
        return false;
    }
    const auto remaining = flags & ~static_cast<DWORD>(DIDFT_FFACTUATOR);
    return remaining == 0U || matches_object_filter(object, remaining);
}

template <typename EffectInfo>
void fill_effect_info(EffectInfo& value,
    const force_feedback::effect_descriptor& descriptor) noexcept {
    value.dwSize = sizeof(value);
    value.guid = *descriptor.id;
    value.dwEffType = descriptor.type;
    value.dwStaticParams = descriptor.static_params;
    value.dwDynamicParams = descriptor.dynamic_params;
    using character = std::remove_extent_t<decltype(value.tszName)>;
    const auto* const name = [&] {
        if constexpr (std::is_same_v<character, char>) {
            return descriptor.name;
        } else {
            return descriptor.wide_name;
        }
    }();
    const auto length =
        std::min<std::size_t>(std::char_traits<character>::length(name), MAX_PATH - 1U);
    std::copy_n(name, length, value.tszName);
}

class device final : public IDirectInputDevice8A, public IDirectInputDevice8W {
public:
    device(const device_identity& identity,
        std::shared_ptr<input_access> access,
        IUnknown& parent,
        native_input_factory factory)
        : identity_(identity),
          access_(std::move(access)),
          lifetime_(parent),
          input_(identity_, access_, std::move(factory)),
          effects_(*static_cast<IDirectInputDevice8A*>(this), access_, input_) {}

    ~device() = default;

    device(const device&) = delete;
    device& operator=(const device&) = delete;
    device(device&&) = delete;
    device& operator=(device&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDirectInputDevice8A) {
            *out = static_cast<IDirectInputDevice8A*>(this);
        } else if (iid == IID_IDirectInputDevice8W) {
            *out = static_cast<IDirectInputDevice8W*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(DIDEVCAPS* out) noexcept override {
        const auto size = requested_info_size<DIDEVCAPS_DX3>(out);
        if (size == 0U) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            const auto call = access_->enter();
            if (!call) {
                return DIERR_INPUTLOST;
            }
            const auto latest = call.core().read_latest(identity_.target);
            const bool attached = latest.has_value() || latest.error() == core::status::no_data;
            if (!attached && latest.error() != core::status::stale_target) {
                return DIERR_INPUTLOST;
            }
            DIDEVCAPS value{};
            value.dwFlags = DIDC_EMULATED | (attached ? DIDC_ATTACHED : 0U);
            if (input_.feedback_capable(call)) {
                value.dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE | DIDC_STARTDELAY;
                value.dwFFSamplePeriod = 4000U;
                value.dwFFMinTimeResolution = 4000U;
            }
            value.dwDevType = identity_.instance.dwDevType;
            value.dwAxes = static_cast<DWORD>(axis_count);
            value.dwButtons = static_cast<DWORD>(button_count);
            value.dwPOVs = 1U;
            write_info(out, value, size);
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA callback,
        void* context,
        DWORD flags) noexcept override {
        return enumerate_objects(callback, context, flags, &describe_object_ansi);
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW callback,
        void* context,
        DWORD flags) noexcept override {
        return enumerate_objects(callback, context, flags, &describe_object);
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property,
        DIPROPHEADER* header) noexcept override {
        const auto* const token = property_token(&property);
        return token == &DIPROP_FFGAIN ? effects_.get_gain(token, header)
                                       : input_.get_property(token, header);
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property,
        const DIPROPHEADER* header) noexcept override {
        const auto* const token = property_token(&property);
        return token == &DIPROP_FFGAIN ? effects_.set_gain(token, header)
                                       : input_.set_property(token, header);
    }

    HRESULT STDMETHODCALLTYPE Acquire() noexcept override {
        const callback_reference retained{*static_cast<IDirectInputDevice8A*>(this)};
        return input_.acquire();
    }

    HRESULT STDMETHODCALLTYPE Unacquire() noexcept override {
        const callback_reference retained{*static_cast<IDirectInputDevice8A*>(this)};
        return input_.unacquire();
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, void* out) noexcept override {
        return input_.get_state(size, out);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size,
        DIDEVICEOBJECTDATA* data,
        DWORD* count,
        DWORD flags) noexcept override {
        return input_.get_data(size, data, count, flags);
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(const DIDATAFORMAT* format) noexcept override {
        return input_.set_format(format);
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) noexcept override {
        return input_.set_notification(event);
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) noexcept override {
        return input_.set_cooperative(window, flags);
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(DIDEVICEOBJECTINSTANCEA* out,
        DWORD object,
        DWORD how) noexcept override {
        return get_object_info<DIDEVICEOBJECTINSTANCE_DX3A>(out,
            object,
            how,
            &describe_object_ansi);
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(DIDEVICEOBJECTINSTANCEW* out,
        DWORD object,
        DWORD how) noexcept override {
        return get_object_info<DIDEVICEOBJECTINSTANCE_DX3W>(out, object, how, &describe_object);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(DIDEVICEINSTANCEW* out) noexcept override {
        const auto size = requested_info_size<DIDEVICEINSTANCE_DX3W>(out);
        if (size == 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (!access_->open()) {
            return DIERR_INPUTLOST;
        }
        write_info(out, identity_.instance, size);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(DIDEVICEINSTANCEA* out) noexcept override {
        const auto size = requested_info_size<DIDEVICEINSTANCE_DX3A>(out);
        if (size == 0U) {
            return DIERR_INVALIDPARAM;
        }
        if (!access_->open()) {
            return DIERR_INPUTLOST;
        }
        DIDEVICEINSTANCEA value{};
        const auto converted = copy_instance_ansi(identity_.instance, value);
        if (FAILED(converted)) {
            return converted;
        }
        write_info(out, value, size);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel([[maybe_unused]] HWND window,
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Initialize([[maybe_unused]] HINSTANCE instance,
        [[maybe_unused]] DWORD version,
        [[maybe_unused]] REFGUID guid) noexcept override {
        return DIERR_ALREADYINITIALIZED;
    }

    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID guid,
        const DIEFFECT* parameters,
        IDirectInputEffect** out,
        IUnknown* outer) noexcept override {
        return effects_.create(guid, parameters, out, outer);
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA callback,
        void* context,
        DWORD flags) noexcept override {
        return enumerate_effects<DIEFFECTINFOA>(callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW callback,
        void* context,
        DWORD flags) noexcept override {
        return enumerate_effects<DIEFFECTINFOW>(callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(DIEFFECTINFOA* out, REFGUID guid) noexcept override {
        return get_effect_info(out, guid);
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(DIEFFECTINFOW* out, REFGUID guid) noexcept override {
        return get_effect_info(out, guid);
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(DWORD* out) noexcept override {
        return effects_.get_state(out);
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD flags) noexcept override {
        return effects_.command(flags);
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(
        LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
        void* context,
        DWORD flags) noexcept override {
        return effects_.enumerate_created(callback, context, flags);
    }

    HRESULT STDMETHODCALLTYPE Escape(DIEFFESCAPE* value) noexcept override {
        return value == nullptr || value->dwSize != sizeof(*value) ? DIERR_INVALIDPARAM
                                                                   : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Poll() noexcept override { return input_.poll(); }

    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD size,
        [[maybe_unused]] const DIDEVICEOBJECTDATA* data,
        DWORD* count,
        DWORD flags) noexcept override {
        return size != sizeof(DIDEVICEOBJECTDATA) || count == nullptr || flags != 0U
                   ? DIERR_INVALIDPARAM
                   : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(const char* file,
        LPDIENUMEFFECTSINFILECALLBACK callback,
        [[maybe_unused]] void* context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return file == nullptr || callback == nullptr ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(const wchar_t* file,
        LPDIENUMEFFECTSINFILECALLBACK callback,
        [[maybe_unused]] void* context,
        [[maybe_unused]] DWORD flags) noexcept override {
        return file == nullptr || callback == nullptr ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(const char* file,
        [[maybe_unused]] DWORD count,
        [[maybe_unused]] DIFILEEFFECT* effects,
        [[maybe_unused]] DWORD flags) noexcept override {
        return file == nullptr ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(const wchar_t* file,
        [[maybe_unused]] DWORD count,
        [[maybe_unused]] DIFILEEFFECT* effects,
        [[maybe_unused]] DWORD flags) noexcept override {
        return file == nullptr ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap(DIACTIONFORMATA* format,
        [[maybe_unused]] const char* user,
        [[maybe_unused]] DWORD flags) noexcept override {
        return unsupported_action(format);
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap(DIACTIONFORMATW* format,
        [[maybe_unused]] const wchar_t* user,
        [[maybe_unused]] DWORD flags) noexcept override {
        return unsupported_action(format);
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(DIACTIONFORMATA* format,
        [[maybe_unused]] const char* user,
        [[maybe_unused]] DWORD flags) noexcept override {
        return unsupported_action(format);
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(DIACTIONFORMATW* format,
        [[maybe_unused]] const wchar_t* user,
        [[maybe_unused]] DWORD flags) noexcept override {
        return unsupported_action(format);
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(DIDEVICEIMAGEINFOHEADERA* out) noexcept override {
        return unsupported_action(out);
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(DIDEVICEIMAGEINFOHEADERW* out) noexcept override {
        return unsupported_action(out);
    }

private:
    template <typename Structure>
    static HRESULT unsupported_action(Structure* value) noexcept {
        return value == nullptr || value->dwSize != sizeof(*value) ? DIERR_INVALIDPARAM
                                                                   : DIERR_UNSUPPORTED;
    }

    template <typename Callback, typename Projection>
    HRESULT enumerate_objects(Callback callback,
        void* context,
        DWORD flags,
        Projection projection) noexcept {
        if (callback == nullptr || !valid_object_filter(flags)) {
            return DIERR_INVALIDPARAM;
        }
        if (!access_->open()) {
            return DIERR_INPUTLOST;
        }
        return guarded([&] -> HRESULT {
            const callback_reference retained{*static_cast<IDirectInputDevice8A*>(this)};
            bool feedback{};
            {
                const auto call = access_->enter();
                if (!call) {
                    return DIERR_INPUTLOST;
                }
                feedback = input_.feedback_capable(call);
            }
            for (const auto& object : objects()) {
                if (matches_device_object(object, flags, feedback)) {
                    const auto info = describe_for_device(object, projection, feedback);
                    if (callback(&info, context) == DIENUM_STOP) {
                        break;
                    }
                }
            }
            return DI_OK;
        });
    }

    template <typename Legacy, typename Info, typename Projection>
    HRESULT get_object_info(Info* out, DWORD object, DWORD how, Projection projection) noexcept {
        const auto size = requested_info_size<Legacy>(out);
        if (size == 0U || how == DIPH_DEVICE || how > DIPH_BYUSAGE) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            if (!access_->open()) {
                return DIERR_INPUTLOST;
            }
            bool feedback{};
            {
                const auto call = access_->enter();
                if (!call) {
                    return DIERR_INPUTLOST;
                }
                feedback = input_.feedback_capable(call);
            }
            const bool requires_actuator = how == DIPH_BYID && (object & DIDFT_FFACTUATOR) != 0U;
            const DWORD selector =
                requires_actuator ? object & ~static_cast<DWORD>(DIDFT_FFACTUATOR) : object;
            const auto selected = input_.select_object(selector, how);
            if (!selected.has_value()) {
                return DIERR_OBJECTNOTFOUND;
            }
            const auto& selected_object = objects()[*selected];
            if (requires_actuator && (!feedback || !is_feedback_actuator(selected_object))) {
                return DIERR_OBJECTNOTFOUND;
            }
            write_info(out, describe_for_device(selected_object, projection, feedback), size);
            return DI_OK;
        });
    }

    template <typename Info, typename Callback>
    HRESULT enumerate_effects(Callback callback, void* context, DWORD flags) noexcept {
        if (callback == nullptr || !valid_effect_filter(flags)) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            bool feedback{};
            {
                const auto call = access_->enter();
                if (!call) {
                    return DIERR_INPUTLOST;
                }
                feedback = input_.feedback_capable(call);
            }
            if (!feedback) {
                return DI_OK;
            }
            const callback_reference retained{*static_cast<IDirectInputDevice8A*>(this)};
            for (const auto& descriptor : force_feedback::effects()) {
                if (!matches_effect_filter(descriptor, flags)) {
                    continue;
                }
                Info info{};
                fill_effect_info(info, descriptor);
                if (callback(&info, context) == DIENUM_STOP) {
                    break;
                }
            }
            return DI_OK;
        });
    }

    template <typename Info>
    HRESULT get_effect_info(Info* out, REFGUID guid) noexcept {
        if (out == nullptr || out->dwSize != sizeof(*out)) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            const auto call = access_->enter();
            if (!call) {
                return DIERR_INPUTLOST;
            }
            if (!input_.feedback_capable(call)) {
                return DIERR_UNSUPPORTED;
            }
            const auto* const descriptor = force_feedback::find_effect(guid);
            if (descriptor == nullptr) {
                return DIERR_UNSUPPORTED;
            }
            Info value{};
            fill_effect_info(value, *descriptor);
            *out = value;
            return DI_OK;
        });
    }

    const device_identity identity_;
    const std::shared_ptr<input_access> access_;
    // Destroy input_ before releasing the parent or removing the device from the unload count.
    const device_lifetime lifetime_;
    std::atomic<ULONG> references_{1U};
    device_input input_;
    force_feedback::device_effects effects_;
};

} // namespace

HRESULT make_device(const device_identity& identity,
    std::shared_ptr<input_access> access,
    IUnknown& parent,
    bool unicode,
    void** out,
    native_input_factory factory) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (!identity.target.valid() || access == nullptr || !access->open()) {
        return DIERR_DEVICENOTREG;
    }
    return guarded([&] -> HRESULT {
        const auto call = access->enter();
        if (!call) {
            return DIERR_DEVICENOTREG;
        }
        const auto latest = call.core().read_latest(identity.target);
        if (!latest.has_value() && latest.error() != core::status::no_data) {
            return DIERR_DEVICENOTREG;
        }
        auto* created =
            new (std::nothrow) device{identity, std::move(access), parent, std::move(factory)};
        if (created == nullptr) {
            return DIERR_OUTOFMEMORY;
        }
        *out = unicode ? static_cast<void*>(static_cast<IDirectInputDevice8W*>(created))
                       : static_cast<void*>(static_cast<IDirectInputDevice8A*>(created));
        return DI_OK;
    });
}

} // namespace inputnext::dinput
