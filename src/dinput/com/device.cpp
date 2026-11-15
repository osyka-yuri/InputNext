#include "device.hpp"

#include "core/core.hpp"
#include "dinput/buffered/event_registration.hpp"
#include "dinput/buffered/service.hpp"
#include "dinput/buffered/subscription.hpp"
#include "dinput/device_info.hpp"
#include "dinput/device_properties.hpp"
#include "dinput/format.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "dinput/profile.hpp"
#include "factory.hpp"
#include <inputnext/focus.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
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

enum class acquisition : std::uint8_t { unacquired, acquired, lost };

struct state_capture final {
    compiled_format format{};
    axis_configuration axes{};
    std::uint64_t generation{};
};

class device final : public IDirectInputDevice8A, public IDirectInputDevice8W {
public:
    device(const device_identity& identity,
        std::shared_ptr<input_access> access,
        IUnknown& parent) noexcept
        : identity_(identity),
          access_(std::move(access)),
          parent_(parent) {
        parent_.AddRef();
        object_created();
    }

    ~device() {
        if (buffered_ != nullptr) {
            buffered_->cancel();
            buffered_->synchronize_signals();
        }
        if (notification_ != nullptr) {
            notification_->disable();
        }
        object_destroyed();
        parent_.Release();
    }

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
        if (header == nullptr || !valid_property_header(*header)) {
            return DIERR_INVALIDPARAM;
        }
        const auto* const token = property_token(&property);
        return guarded([&] -> HRESULT {
            if (!access_->open()) {
                return DIERR_INPUTLOST;
            }
            std::scoped_lock lock{gate_};
            return read_device_property(token,
                header,
                identity_,
                active_format(),
                axes_,
                requested_buffer_size_);
        });
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property,
        const DIPROPHEADER* header) noexcept override {
        if (header == nullptr || !valid_property_header(*header)) {
            return DIERR_INVALIDPARAM;
        }
        const auto* const token = property_token(&property);
        property_change change{};
        const auto decoded = decode_property_change(token, *header, change);
        if (FAILED(decoded)) {
            return decoded;
        }
        return guarded([&] -> HRESULT {
            std::scoped_lock lock{gate_};
            const auto allowed = configure_allowed();
            if (FAILED(allowed)) {
                return allowed;
            }
            auto candidate = axes_;
            const auto updated = apply_property_change(change, *header, active_format(), candidate);
            if (FAILED(updated)) {
                return updated;
            }
            if (change.kind == property_change::setting::buffer_size) {
                if (change.value != requested_buffer_size_ && !advance_generation()) {
                    return DIERR_GENERIC;
                }
                requested_buffer_size_ = change.value;
                return DI_OK;
            }
            if (!change.affects_axes()) {
                return DI_OK;
            }
            if (!advance_generation()) {
                return DIERR_GENERIC;
            }
            axes_ = candidate;
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE Acquire() noexcept override {
        return guarded([&] { return acquire(); });
    }

    HRESULT STDMETHODCALLTYPE Unacquire() noexcept override {
        return guarded([&] -> HRESULT {
            std::shared_ptr<buffered_subscription> retired;
            {
                std::scoped_lock lock{gate_};
                retired = std::move(buffered_);
                if (retired != nullptr) {
                    retired->cancel();
                }
                static_cast<void>(advance_generation());
                acquisition_ = acquisition::unacquired;
                acquired_window_.reset();
            }
            if (retired != nullptr) {
                retired->synchronize_signals();
            }
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, void* out) noexcept override {
        if (out == nullptr || size == 0U) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            const auto call = access_->enter();
            if (!call) {
                return DIERR_INPUTLOST;
            }
            state_capture captured{};
            const auto ready = capture_state(call, size, captured);
            if (FAILED(ready)) {
                return ready;
            }
            const auto latest = call.core().read_latest(identity_.target);
            if (!latest.has_value()) {
                if (latest.error() == core::status::no_data) {
                    const auto valid = revalidate(call, captured.generation);
                    return FAILED(valid) ? valid : E_PENDING;
                }
                return lose(captured.generation);
            }
            std::array<std::byte, max_format_data_size> projected{};
            const auto written = write_state(captured.format,
                project(latest->sample.state, captured.axes),
                std::span{projected}.first(size));
            if (FAILED(written)) {
                return written;
            }
            const auto valid = revalidate(call, captured.generation);
            if (FAILED(valid)) {
                return valid;
            }
            std::memcpy(out, projected.data(), size);
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size,
        DIDEVICEOBJECTDATA* data,
        DWORD* count,
        DWORD flags) noexcept override {
        constexpr DWORD allowed_flags = DIGDD_PEEK;
        if (count == nullptr ||
            (size != sizeof(DIDEVICEOBJECTDATA) && size != sizeof(DIDEVICEOBJECTDATA_DX3)) ||
            (flags & ~allowed_flags) != 0U) {
            return DIERR_INVALIDPARAM;
        }
        const DWORD requested = *count;
        *count = 0U;
        return guarded([&] -> HRESULT {
            const auto call = access_->enter();
            if (!call) {
                return DIERR_INPUTLOST;
            }
            std::uint64_t generation{};
            const auto captured = capture_live_acquisition(call, generation);
            if (FAILED(captured)) {
                return captured;
            }
            std::scoped_lock lock{gate_};
            const auto ready = revalidate_locked(call, generation);
            if (FAILED(ready)) {
                return ready;
            }
            if (requested_buffer_size_ == 0U || buffered_ == nullptr) {
                return DIERR_NOTBUFFERED;
            }
            return buffered_->read(size, data, requested, *count, (flags & DIGDD_PEEK) != 0U);
        });
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(const DIDATAFORMAT* format) noexcept override {
        if (format == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        return guarded([&] -> HRESULT {
            compiled_format compiled{};
            const auto outcome = compile_format(*format, compiled);
            if (FAILED(outcome)) {
                return outcome;
            }
            std::scoped_lock lock{gate_};
            const auto allowed = configure_allowed();
            if (FAILED(allowed)) {
                return allowed;
            }
            if (!advance_generation()) {
                return DIERR_GENERIC;
            }
            format_ = compiled;
            has_format_ = true;
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) noexcept override {
        return guarded([&] -> HRESULT {
            {
                std::scoped_lock lock{gate_};
                const auto allowed = configure_allowed();
                if (FAILED(allowed)) {
                    return allowed;
                }
            }
            auto registration = event_registration::duplicate(event);
            if (!registration.has_value()) {
                return registration.error();
            }
            std::shared_ptr<event_registration> previous;
            {
                std::scoped_lock lock{gate_};
                const auto allowed = configure_allowed();
                if (FAILED(allowed)) {
                    return allowed;
                }
                if (!advance_generation()) {
                    return DIERR_GENERIC;
                }
                previous = std::exchange(notification_, std::move(*registration));
            }
            if (previous != nullptr) {
                previous->disable();
            }
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) noexcept override {
        constexpr DWORD modes =
            DISCL_FOREGROUND | DISCL_BACKGROUND | DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE;
        if ((flags & ~modes) != 0U ||
            ((flags & DISCL_FOREGROUND) != 0U) == ((flags & DISCL_BACKGROUND) != 0U) ||
            ((flags & DISCL_EXCLUSIVE) != 0U) == ((flags & DISCL_NONEXCLUSIVE) != 0U)) {
            return DIERR_INVALIDPARAM;
        }
        DWORD process{};
        if (window == nullptr || IsWindow(window) == FALSE ||
            GetAncestor(window, GA_ROOT) != window ||
            GetWindowThreadProcessId(window, &process) == 0U || process != GetCurrentProcessId()) {
            return E_HANDLE;
        }
        return guarded([&] -> HRESULT {
            std::scoped_lock lock{gate_};
            const auto allowed = configure_allowed();
            if (FAILED(allowed)) {
                return allowed;
            }
            if ((flags & DISCL_EXCLUSIVE) != 0U) {
                return DIERR_UNSUPPORTED;
            }
            if (!advance_generation()) {
                return DIERR_GENERIC;
            }
            window_ = window;
            cooperation_ = flags;
            return DI_OK;
        });
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

    HRESULT STDMETHODCALLTYPE Poll() noexcept override {
        return guarded([&] -> HRESULT { return poll_eligibility(); });
    }

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
    HRESULT acquire() {
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        std::uint64_t expected{};
        HWND window{};
        bool foreground{};
        {
            std::scoped_lock lock{gate_};
            if (!has_format_ || cooperation_ == 0U) {
                return DIERR_INVALIDPARAM;
            }
            if (acquiring_) {
                return DIERR_ACQUIRED;
            }
            expected = generation_;
            window = window_;
            foreground = (cooperation_ & DISCL_FOREGROUND) != 0U;
        }
        if (!target_available(call)) {
            return lose(expected);
        }
        std::unique_lock lock{gate_};
        if (generation_ != expected || !access_->open()) {
            return DIERR_INPUTLOST;
        }
        observe_buffer_loss();
        std::optional<focus::window_epoch> epoch;
        if (foreground) {
            const auto guard = call.focus().lock_presentation();
            epoch =
                guard.current_foreground_window_epoch(reinterpret_cast<focus::window_id>(window));
            if (!epoch.has_value()) {
                if (acquisition_ == acquisition::acquired) {
                    mark_lost();
                }
                return DIERR_OTHERAPPHASPRIO;
            }
        }
        if (acquisition_ == acquisition::acquired && acquired_window_ == epoch) {
            return S_FALSE;
        }
        if (acquisition_ == acquisition::acquired) {
            mark_lost();
        }
        if (requested_buffer_size_ != 0U || notification_ != nullptr) {
            return acquire_buffered(call, epoch, lock);
        }
        if (!advance_generation()) {
            return DIERR_GENERIC;
        }
        acquired_window_ = epoch;
        acquisition_ = acquisition::acquired;
        return DI_OK;
    }

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
            std::optional<std::size_t> selected;
            {
                std::scoped_lock lock{gate_};
                selected = find_object(object, how, active_format());
            }
            if (!selected.has_value()) {
                return DIERR_OBJECTNOTFOUND;
            }
            write_info(out, projection(objects()[*selected]), size);
            return DI_OK;
        });
    }

    [[nodiscard]] bool target_available(const input_access::call& call) const {
        const auto latest = call.core().read_latest(identity_.target);
        return latest.has_value() || latest.error() == core::status::no_data;
    }

    [[nodiscard]] static bool window_matches(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch) noexcept {
        if (!epoch.has_value()) {
            return true;
        }
        const auto guard = call.focus().lock_presentation();
        return guard.matches_foreground_window_epoch(*epoch);
    }

    HRESULT acquire_buffered(const input_access::call& call,
        const std::optional<focus::window_epoch>& epoch,
        std::unique_lock<std::mutex>& lock) {
        if (!advance_generation()) {
            return DIERR_GENERIC;
        }
        const auto expected = generation_;
        acquiring_ = true;
        auto retired = buffered_;
        if (retired != nullptr) {
            retired->cancel();
        }
        buffer_configuration configuration{.target = identity_.target,
            .format = format_,
            .axes = axes_,
            .requested_size = requested_buffer_size_,
            .notification = notification_,
            .window = epoch};
        lock.unlock();
        if (retired != nullptr) {
            retired->synchronize_signals();
            lock.lock();
            if (generation_ != expected || !access_->open()) {
                if (generation_ == expected) {
                    mark_lost();
                }
                return DIERR_INPUTLOST;
            }
            buffered_.reset();
            lock.unlock();
            retired.reset();
        }
        const auto pending = call.buffers().subscribe(std::move(configuration));
        lock.lock();
        if (generation_ != expected || !access_->open()) {
            if (pending.has_value()) {
                (*pending)->cancel();
            }
            if (generation_ == expected) {
                mark_lost();
            }
            return DIERR_INPUTLOST;
        }
        if (!pending.has_value()) {
            acquiring_ = false;
            if (pending.error() == DIERR_INPUTLOST) {
                mark_lost();
            }
            return pending.error();
        }
        buffered_ = *pending;

        lock.unlock();
        const auto ready = guarded([&] { return (*pending)->wait_ready(); });
        const bool live = SUCCEEDED(ready) && target_available(call);
        lock.lock();
        if (generation_ != expected) {
            return DIERR_INPUTLOST;
        }
        acquiring_ = false;
        if (FAILED(ready) && access_->open() && ready != DIERR_INPUTLOST) {
            buffered_->cancel();
            buffered_.reset();
            return ready;
        }
        if (!live || !access_->open() || !window_matches(call, epoch) || !buffered_->activate()) {
            mark_lost();
            return DIERR_INPUTLOST;
        }
        acquired_window_ = epoch;
        acquisition_ = acquisition::acquired;
        lock.unlock();
        (*pending)->signal_pending();
        return DI_OK;
    }

    [[nodiscard]] bool advance_generation() noexcept {
        if (acquiring_) {
            acquiring_ = false;
            if (buffered_ != nullptr) {
                buffered_->cancel();
                buffered_.reset();
            }
        }
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            acquisition_ = acquisition::lost;
            return false;
        }
        ++generation_;
        return true;
    }

    void mark_lost() noexcept {
        acquisition_ = acquisition::lost;
        if (buffered_ != nullptr) {
            buffered_->lose();
        }
        static_cast<void>(advance_generation());
        acquired_window_.reset();
    }

    void observe_buffer_loss() noexcept {
        if (acquisition_ == acquisition::acquired && buffered_ != nullptr && !buffered_->active()) {
            mark_lost();
        }
    }

    HRESULT lose(std::uint64_t expected) {
        std::scoped_lock lock{gate_};
        if (generation_ == expected) {
            mark_lost();
        }
        return DIERR_INPUTLOST;
    }

    [[nodiscard]] HRESULT configure_allowed() noexcept {
        if (!access_->open()) {
            return DIERR_INPUTLOST;
        }
        observe_buffer_loss();
        return acquisition_ == acquisition::acquired ? DIERR_ACQUIRED : DI_OK;
    }

    // Requires gate_. The window check also qualifies the process epoch and freshness.
    HRESULT check_acquisition(const input_access::call& call) {
        observe_buffer_loss();
        if (acquisition_ != acquisition::acquired) {
            return acquisition_ == acquisition::lost ? DIERR_INPUTLOST : DIERR_NOTACQUIRED;
        }
        if (!window_matches(call, acquired_window_)) {
            mark_lost();
            return DIERR_INPUTLOST;
        }
        return DI_OK;
    }

    HRESULT capture_state(const input_access::call& call, DWORD size, state_capture& out) {
        std::scoped_lock lock{gate_};
        if (!has_format_ || size != format_.data_size) {
            return DIERR_INVALIDPARAM;
        }
        const auto eligible = check_acquisition(call);
        if (FAILED(eligible)) {
            return eligible;
        }
        out.generation = generation_;
        out.format = format_;
        out.axes = axes_;
        return DI_OK;
    }

    HRESULT revalidate(const input_access::call& call, std::uint64_t generation) {
        std::scoped_lock lock{gate_};
        return revalidate_locked(call, generation);
    }

    HRESULT revalidate_locked(const input_access::call& call, std::uint64_t generation) {
        if (!access_->open() || generation_ != generation) {
            return acquisition_ == acquisition::unacquired ? DIERR_NOTACQUIRED : DIERR_INPUTLOST;
        }
        return check_acquisition(call);
    }

    HRESULT capture_live_acquisition(const input_access::call& call, std::uint64_t& generation) {
        {
            std::scoped_lock lock{gate_};
            const auto eligible = check_acquisition(call);
            if (FAILED(eligible)) {
                return eligible;
            }
            generation = generation_;
        }
        return target_available(call) ? DI_OK : lose(generation);
    }

    HRESULT poll_eligibility() {
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        std::uint64_t generation{};
        const auto captured = capture_live_acquisition(call, generation);
        if (FAILED(captured)) {
            return captured;
        }
        const auto valid = revalidate(call, generation);
        return FAILED(valid) ? valid : DI_NOEFFECT;
    }

    // Requires gate_. The returned view is borrowed only during the current operation.
    [[nodiscard]] const compiled_format* active_format() const noexcept {
        return has_format_ ? &format_ : nullptr;
    }

    const device_identity identity_;
    const std::shared_ptr<input_access> access_;
    IUnknown& parent_;
    std::atomic<ULONG> references_{1U};
    mutable std::mutex gate_;
    compiled_format format_{};
    axis_configuration axes_{};
    std::shared_ptr<buffered_subscription> buffered_;
    std::shared_ptr<event_registration> notification_;
    HWND window_{};
    DWORD cooperation_{};
    DWORD requested_buffer_size_{};
    std::uint64_t generation_{1U};
    acquisition acquisition_{acquisition::unacquired};
    std::optional<focus::window_epoch> acquired_window_;
    bool has_format_{};
    bool acquiring_{};
};

} // namespace

HRESULT make_device(const device_identity& identity,
    std::shared_ptr<input_access> access,
    IUnknown& parent,
    bool unicode,
    void** out) noexcept {
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
        auto* created = new (std::nothrow) device{identity, std::move(access), parent};
        if (created == nullptr) {
            return DIERR_OUTOFMEMORY;
        }
        *out = unicode ? static_cast<void*>(static_cast<IDirectInputDevice8W*>(created))
                       : static_cast<void*>(static_cast<IDirectInputDevice8A*>(created));
        return DI_OK;
    });
}

} // namespace inputnext::dinput
