#include "device.hpp"

#include "core/core.hpp"
#include "dinput/device_info.hpp"
#include "dinput/device_input.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/profile.hpp"
#include "factory.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
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

class device final : public IDirectInputDevice8A, public IDirectInputDevice8W {
public:
    device(const device_identity& identity,
        std::shared_ptr<input_access> access,
        IUnknown& parent,
        native_input_factory factory)
        : identity_(identity),
          access_(std::move(access)),
          lifetime_(parent),
          input_(identity_, access_, std::move(factory)) {}

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
        return input_.get_property(property_token(&property), header);
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property,
        const DIPROPHEADER* header) noexcept override {
        return input_.set_property(property_token(&property), header);
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

    HRESULT STDMETHODCALLTYPE CreateEffect([[maybe_unused]] REFGUID guid,
        [[maybe_unused]] const DIEFFECT* effect,
        IDirectInputEffect** out,
        IUnknown* outer) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        return outer != nullptr ? CLASS_E_NOAGGREGATION : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA callback,
        [[maybe_unused]] void* context,
        DWORD flags) noexcept override {
        return callback == nullptr || !valid_effect_filter(flags) ? DIERR_INVALIDPARAM : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW callback,
        [[maybe_unused]] void* context,
        DWORD flags) noexcept override {
        return callback == nullptr || !valid_effect_filter(flags) ? DIERR_INVALIDPARAM : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(DIEFFECTINFOA* out,
        [[maybe_unused]] REFGUID guid) noexcept override {
        return out == nullptr || out->dwSize != sizeof(*out) ? DIERR_INVALIDPARAM
                                                             : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(DIEFFECTINFOW* out,
        [[maybe_unused]] REFGUID guid) noexcept override {
        return out == nullptr || out->dwSize != sizeof(*out) ? DIERR_INVALIDPARAM
                                                             : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(DWORD* out) noexcept override {
        return out == nullptr ? DIERR_INVALIDPARAM : DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(
        [[maybe_unused]] DWORD flags) noexcept override {
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(
        LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback,
        [[maybe_unused]] void* context,
        DWORD flags) noexcept override {
        return callback == nullptr || flags != 0U ? DIERR_INVALIDPARAM : DI_OK;
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
            for (const auto& object : objects()) {
                if (matches_object_filter(object, flags)) {
                    const auto info = projection(object);
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
            const auto selected = input_.select_object(object, how);
            if (!selected.has_value()) {
                return DIERR_OBJECTNOTFOUND;
            }
            write_info(out, projection(objects()[*selected]), size);
            return DI_OK;
        });
    }

    const device_identity identity_;
    const std::shared_ptr<input_access> access_;
    // Destroy input_ before releasing the parent or removing the device from the unload count.
    const device_lifetime lifetime_;
    std::atomic<ULONG> references_{1U};
    device_input input_;
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
