#include "device_input.hpp"
#include "core/core.hpp"
#include "device_info.hpp"
#include "device_properties.hpp"
#include "dinput/buffered/event_registration.hpp"
#include "dinput/buffered/service.hpp"
#include "dinput/buffered/subscription.hpp"
#include "dinput/exclusive/lease.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/force_feedback/service.hpp"
#include "format.hpp"
#include "input_access.hpp"
#include "native_types.hpp"
#include "profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace inputnext::dinput {
device_input::device_input(const device_identity& identity,
    std::shared_ptr<input_access> access,
    native_input_factory factory)
    : identity_(identity),
      access_(std::move(access)),
      factory_(std::move(factory)) {
}

device_input::~device_input() {
    if (exclusive_ != nullptr) {
        exclusive_->cancel();
    }
    if (retiring_ != nullptr) {
        retiring_->cancel();
    }
    if (buffered_ != nullptr) {
        buffered_->cancel();
        buffered_->synchronize_signals();
    }
    if (notification_ != nullptr) {
        notification_->disable();
    }
}

std::optional<std::size_t> device_input::select_object(DWORD object, DWORD how) const noexcept {
    std::scoped_lock lock{gate_};
    return find_object(object, how, active_format(), access_->model());
}

HRESULT device_input::get_property(const GUID* property, DIPROPHEADER* header) noexcept {
    if (header == nullptr || !valid_property_header(*header)) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        if (!access_->open()) {
            return DIERR_INPUTLOST;
        }
        std::scoped_lock lock{gate_};
        return read_device_property(property,
            header,
            identity_,
            active_format(),
            axes_,
            requested_buffer_size_,
            access_->model());
    });
}

HRESULT device_input::set_property(const GUID* property, const DIPROPHEADER* header) noexcept {
    if (header == nullptr || !valid_property_header(*header)) {
        return DIERR_INVALIDPARAM;
    }
    property_change change{};
    const auto decoded = decode_property_change(property, *header, change);
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
        const auto updated =
            apply_property_change(change, *header, active_format(), candidate, access_->model());
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

HRESULT device_input::acquire() noexcept {
    return guarded([&] { return acquire_impl(); });
}

HRESULT device_input::unacquire() noexcept {
    return guarded([&] -> HRESULT {
        std::unique_lock lock{gate_};
        const bool pending = acquiring_ && exclusive_ != nullptr;
        static_cast<void>(advance_generation());
        retire_exclusive();
        const auto closing = retiring_;
        acquisition_ = acquisition::unacquired;
        loss_ = DIERR_INPUTLOST;
        acquired_window_.reset();
        discard_buffered_subscription(lock);
        lock.unlock();
        if (closing == nullptr || pending) {
            return DI_OK;
        }
        const auto completed = closing->wait_closed();
        lock.lock();
        if (retiring_ == closing && closing->closed()) {
            retiring_.reset();
        }
        return completed;
    });
}

HRESULT device_input::get_state(DWORD size, void* out) noexcept {
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
        const auto& model = access_->model();
        const auto written = write_state(captured.format,
            model.project(latest->sample.state, captured.axes),
            std::span{projected}.first(size),
            model);
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

HRESULT
device_input::get_data(DWORD size, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags) noexcept {
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

HRESULT device_input::set_format(const DIDATAFORMAT* format) noexcept {
    if (format == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    return guarded([&] -> HRESULT {
        compiled_format compiled{};
        const auto call = access_->enter();
        if (!call) {
            return DIERR_INPUTLOST;
        }
        const auto outcome =
            compile_format(*format, compiled, access_->model(), feedback_capable(call));
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

HRESULT device_input::set_notification(HANDLE event) noexcept {
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

HRESULT device_input::set_cooperative(HWND window, DWORD flags) noexcept {
    constexpr DWORD modes =
        DISCL_FOREGROUND | DISCL_BACKGROUND | DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE;
    if ((flags & ~modes) != 0U ||
        ((flags & DISCL_FOREGROUND) != 0U) == ((flags & DISCL_BACKGROUND) != 0U) ||
        ((flags & DISCL_EXCLUSIVE) != 0U) == ((flags & DISCL_NONEXCLUSIVE) != 0U)) {
        return DIERR_INVALIDPARAM;
    }
    DWORD process{};
    if (window == nullptr || IsWindow(window) == FALSE || GetAncestor(window, GA_ROOT) != window) {
        return E_HANDLE;
    }
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (thread == 0U || process != GetCurrentProcessId()) {
        return E_HANDLE;
    }
    return guarded([&] -> HRESULT {
        std::scoped_lock lock{gate_};
        const auto allowed = configure_allowed();
        if (FAILED(allowed)) {
            return allowed;
        }
        if ((flags & DISCL_EXCLUSIVE) != 0U &&
            (!identity_.path.has_value() || !factory_.supported())) {
            return DIERR_UNSUPPORTED;
        }
        if (!advance_generation()) {
            return DIERR_GENERIC;
        }
        window_ = window;
        window_thread_ = thread;
        cooperation_ = flags;
        return DI_OK;
    });
}

HRESULT device_input::poll() noexcept {
    return guarded([&] -> HRESULT { return poll_eligibility(); });
}

HRESULT
device_input::capture_state(const input_access::call& call, DWORD size, state_capture& out) {
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

HRESULT device_input::revalidate(const input_access::call& call, std::uint64_t generation) {
    std::scoped_lock lock{gate_};
    return revalidate_locked(call, generation);
}

HRESULT device_input::revalidate_locked(const input_access::call& call, std::uint64_t generation) {
    if (!access_->open() || generation_ != generation) {
        return acquisition_error();
    }
    return check_acquisition(call);
}

HRESULT device_input::capture_live_acquisition(const input_access::call& call,
    std::uint64_t& generation) {
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

HRESULT device_input::poll_eligibility() {
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

const compiled_format* device_input::active_format() const noexcept {
    return has_format_ ? &format_ : nullptr;
}

device_input::feedback_capture device_input::capture_feedback(
    const input_access::call& call) noexcept {
    feedback_capture result{};
    std::scoped_lock lock{gate_};
    result.format = format_;
    result.has_format = has_format_;
    if (!access_->open()) {
        result.status = DIERR_INPUTLOST;
        return result;
    }
    const auto acquired = check_acquisition(call);
    if (FAILED(acquired)) {
        result.status = acquisition_ == acquisition::unacquired
                            ? static_cast<HRESULT>(DIERR_NOTEXCLUSIVEACQUIRED)
                            : acquired;
        return result;
    }
    if (exclusive_ == nullptr || !acquired_window_.has_value()) {
        return result;
    }
    const auto token = exclusive_->rumble_window(identity_.target, *acquired_window_);
    if (!token.has_value()) {
        result.status = token.error();
        return result;
    }
    result.token = *token;
    result.status = DI_OK;
    return result;
}

bool device_input::feedback_capable(const input_access::call& call) const noexcept {
    return feedback_available(call, identity_, factory_);
}

bool feedback_available(const input_access::call& call,
    const device_identity& identity,
    const native_input_factory& factory) noexcept {
    if (!identity.path.has_value() || !factory.supported() || call.feedback() == nullptr) {
        return false;
    }
    const auto motors = call.feedback()->capabilities(identity.target);
    return motors.low || motors.high;
}
} // namespace inputnext::dinput
