#include "control_wait.hpp"
#include "core/core.hpp"
#include "device_input.hpp"
#include "dinput/buffered/service.hpp"
#include "dinput/buffered/subscription.hpp"
#include "dinput/exclusive/lease.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "input_access.hpp"
#include "native_types.hpp"
#include <inputnext/focus.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::dinput {

HRESULT device_input::acquire_impl() {
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
    observe_acquisition_loss();
    if (acquisition_ == acquisition::lost && loss_ == DIERR_NOTACQUIRED) {
        return loss_;
    }
    std::optional<focus::window_epoch> epoch;
    if (foreground) {
        const auto guard = call.focus().lock_presentation();
        epoch = guard.current_foreground_window_epoch(reinterpret_cast<focus::window_id>(window));
        if (!epoch.has_value()) {
            if (acquisition_ == acquisition::acquired) {
                mark_lost();
            }
            return DIERR_OTHERAPPHASPRIO;
        }
    }
    if (!access_->open()) {
        return DIERR_INPUTLOST;
    }
    if (acquisition_ == acquisition::acquired && acquired_window_ == epoch) {
        return S_FALSE;
    }
    if (acquisition_ == acquisition::acquired) {
        mark_lost();
    }
    if ((cooperation_ & DISCL_EXCLUSIVE) != 0U) {
        return acquire_exclusive(call, epoch, std::move(lock));
    }
    if (requested_buffer_size_ != 0U || notification_ != nullptr) {
        return acquire_buffered(call, epoch, std::move(lock));
    }
    return acquire_immediate(call, epoch, std::move(lock));
}

HRESULT device_input::acquire_immediate(const input_access::call& call,
    const std::optional<focus::window_epoch>& epoch,
    std::unique_lock<std::mutex> lock) {
    const auto pending = begin_acquisition(lock);
    if (!pending.has_value()) {
        return pending.error();
    }
    lock.unlock();
    const bool live = target_available(call);
    lock.lock();
    if (generation_ != *pending) {
        return DIERR_INPUTLOST;
    }
    if (!live || !window_matches(call, epoch) || !access_->open()) {
        mark_lost();
        return DIERR_INPUTLOST;
    }
    acquiring_ = false;
    acquired_window_ = epoch;
    acquisition_ = acquisition::acquired;
    return DI_OK;
}

std::expected<std::uint64_t, HRESULT> device_input::begin_acquisition(
    std::unique_lock<std::mutex>& lock) {
    if (!advance_generation()) {
        return std::unexpected{DIERR_GENERIC};
    }
    const auto expected = generation_;
    acquiring_ = true;
    discard_buffered_subscription(lock);
    if (generation_ != expected) {
        return std::unexpected{DIERR_INPUTLOST};
    }
    if (!access_->open()) {
        mark_lost();
        return std::unexpected{DIERR_INPUTLOST};
    }
    return expected;
}

void device_input::discard_buffered_subscription(std::unique_lock<std::mutex>& lock) {
    const auto previous = buffered_;
    if (previous == nullptr) {
        return;
    }
    // Keep the canceled subscription visible to concurrent calls until its signal is fenced.
    previous->cancel();
    lock.unlock();
    previous->synchronize_signals();
    lock.lock();
    if (buffered_ == previous) {
        buffered_.reset();
    }
}

struct device_input::exclusive_attempt final {
    exclusive_attempt() = default;
    exclusive_attempt(const exclusive_attempt&) = delete;
    exclusive_attempt& operator=(const exclusive_attempt&) = delete;
    exclusive_attempt(exclusive_attempt&&) = delete;
    exclusive_attempt& operator=(exclusive_attempt&&) = delete;
    std::shared_ptr<exclusive_lease> native;
    std::shared_ptr<buffered_subscription> buffer;
    bool committed{};

    ~exclusive_attempt() { cancel(); }

    void cancel() const noexcept {
        if (committed) {
            return;
        }
        if (native != nullptr) {
            native->cancel();
        }
        if (buffer != nullptr) {
            buffer->cancel();
        }
    }
};

HRESULT device_input::acquire_exclusive(const input_access::call& call,
    const std::optional<focus::window_epoch>& epoch,
    std::unique_lock<std::mutex> lock) {
    if (native_control_reentrant() || (retiring_ != nullptr && !retiring_->closed())) {
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    retiring_.reset();
    if (!identity_.path.has_value() || !factory_.supported()) {
        return DIERR_UNSUPPORTED;
    }
    const auto pending = begin_acquisition(lock);
    if (!pending.has_value()) {
        return pending.error();
    }
    const auto expected = *pending;
    const exclusive_request request{.target = identity_.target,
        .native_instance = identity_.instance.guidInstance,
        .native_path = *identity_.path,
        .window = epoch.value_or(focus::window_epoch{}),
        .factory = factory_,
        .background_window = epoch.has_value() ? background_exclusive_window{}
                                               : background_exclusive_window{.handle = window_,
                                                     .owner_thread = window_thread_}};
    const bool buffered = requested_buffer_size_ != 0U || notification_ != nullptr;
    lock.unlock();
    exclusive_attempt attempt;
    auto completed =
        guarded([&] { return complete_exclusive(call, request, expected, buffered, attempt); });
    if (FAILED(completed)) {
        std::scoped_lock rollback_lock{gate_};
        if (generation_ == expected) {
            retire_exclusive();
            acquiring_ = false;
            if (attempt.native != nullptr && retiring_ == attempt.native &&
                retiring_->authority_status() == DIERR_NOTACQUIRED) {
                loss_ = DIERR_NOTACQUIRED;
                mark_lost();
                completed = loss_;
            }
        }
        if (buffered_ == attempt.buffer) {
            buffered_.reset();
        }
    }
    return completed;
}

HRESULT device_input::complete_exclusive(const input_access::call& call,
    const exclusive_request& request,
    std::uint64_t expected,
    bool buffered,
    exclusive_attempt& attempt) {
    const auto epoch =
        request.background_window.handle == nullptr ? std::optional{request.window} : std::nullopt;
    const auto reserved = call.exclusives().reserve(request);
    if (!reserved.has_value()) {
        return reserved.error();
    }
    attempt.native = *reserved;
    {
        std::scoped_lock lock{gate_};
        if (generation_ != expected || !window_matches(call, epoch) || !access_->open()) {
            return DIERR_INPUTLOST;
        }
        exclusive_ = attempt.native;
    }
    const auto started = attempt.native->start();
    if (FAILED(started)) {
        return started;
    }
    const auto ready = attempt.native->wait_prepared();
    if (FAILED(ready)) {
        return ready;
    }
    if (buffered) {
        const auto history_ready = prepare_exclusive_buffer(call, epoch, expected, attempt);
        if (FAILED(history_ready)) {
            return history_ready;
        }
    }
    const bool live = target_available(call);
    {
        std::scoped_lock lock{gate_};
        if (generation_ != expected || !live || !window_matches(call, epoch) || !access_->open() ||
            !attempt.native->try_activate() || (buffered && !attempt.buffer->activate())) {
            if (generation_ == expected) {
                mark_lost();
                return loss_;
            }
            return DIERR_INPUTLOST;
        }
        acquiring_ = false;
        acquired_window_ = epoch;
        acquisition_ = acquisition::acquired;
        attempt.committed = true;
    }
    if (attempt.buffer != nullptr) {
        attempt.buffer->signal_pending();
    }
    return DI_OK;
}

HRESULT device_input::prepare_exclusive_buffer(const input_access::call& call,
    const std::optional<focus::window_epoch>& epoch,
    std::uint64_t expected,
    exclusive_attempt& attempt) {
    buffer_configuration configuration{};
    {
        std::scoped_lock lock{gate_};
        if (generation_ != expected || !access_->open() || exclusive_ != attempt.native) {
            return DIERR_INPUTLOST;
        }
        const auto authority = attempt.native->authority_status();
        if (FAILED(authority)) {
            return authority;
        }
        configuration = buffer_configuration{.target = identity_.target,
            .format = format_,
            .axes = axes_,
            .requested_size = requested_buffer_size_,
            .notification = notification_,
            .window = epoch,
            .authority = epoch.has_value() ? nullptr : attempt.native};
    }
    const auto subscription = call.buffers().subscribe(std::move(configuration));
    if (!subscription.has_value()) {
        return subscription.error();
    }
    attempt.buffer = *subscription;
    {
        std::scoped_lock lock{gate_};
        if (generation_ != expected || !access_->open()) {
            return DIERR_INPUTLOST;
        }
        buffered_ = attempt.buffer;
    }
    const auto ready = attempt.buffer->wait_ready(attempt.native->cancellation_event());
    const auto authority = attempt.native->authority_status();
    return FAILED(authority) ? authority : ready;
}

void device_input::retire_exclusive() noexcept {
    if (exclusive_ != nullptr) {
        exclusive_->cancel();
        retiring_ = std::move(exclusive_);
    }
}

bool device_input::target_available(const input_access::call& call) const {
    const auto latest = call.core().read_latest(identity_.target);
    return latest.has_value() || latest.error() == core::status::no_data;
}

bool device_input::window_matches(const input_access::call& call,
    const std::optional<focus::window_epoch>& epoch) noexcept {
    if (!epoch.has_value()) {
        return true;
    }
    const auto guard = call.focus().lock_presentation();
    return guard.matches_foreground_window_epoch(*epoch);
}

HRESULT device_input::acquire_buffered(const input_access::call& call,
    const std::optional<focus::window_epoch>& epoch,
    std::unique_lock<std::mutex> lock) {
    const auto generation = begin_acquisition(lock);
    if (!generation.has_value()) {
        return generation.error();
    }
    const auto expected = *generation;
    buffer_configuration configuration{.target = identity_.target,
        .format = format_,
        .axes = axes_,
        .requested_size = requested_buffer_size_,
        .notification = notification_,
        .window = epoch};
    lock.unlock();
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
        if (buffered_ == *pending) {
            buffered_.reset();
        }
        return DIERR_INPUTLOST;
    }
    acquiring_ = false;
    if (FAILED(ready) && access_->open() && ready != DIERR_INPUTLOST) {
        buffered_->cancel();
        buffered_.reset();
        return ready;
    }
    if (!live || !window_matches(call, epoch) || !access_->open() || !buffered_->activate()) {
        mark_lost();
        return DIERR_INPUTLOST;
    }
    acquired_window_ = epoch;
    acquisition_ = acquisition::acquired;
    lock.unlock();
    (*pending)->signal_pending();
    return DI_OK;
}

bool device_input::advance_generation() noexcept {
    if (acquiring_) {
        acquiring_ = false;
        retire_exclusive();
        if (buffered_ != nullptr) {
            buffered_->cancel();
        }
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        acquisition_ = acquisition::lost;
        return false;
    }
    ++generation_;
    return true;
}

void device_input::mark_lost() noexcept {
    const auto lost = exclusive_;
    retire_exclusive();
    if (lost != nullptr && lost->authority_status() == DIERR_NOTACQUIRED) {
        loss_ = DIERR_NOTACQUIRED;
    }
    acquisition_ = acquisition::lost;
    if (buffered_ != nullptr) {
        buffered_->lose();
    }
    static_cast<void>(advance_generation());
    acquired_window_.reset();
}

HRESULT device_input::acquisition_error() const noexcept {
    if (acquisition_ == acquisition::unacquired) {
        return DIERR_NOTACQUIRED;
    }
    return acquisition_ == acquisition::lost ? loss_ : DIERR_INPUTLOST;
}

void device_input::observe_acquisition_loss() noexcept {
    if (acquisition_ == acquisition::acquired &&
        ((exclusive_ != nullptr && !exclusive_->live()) ||
            (buffered_ != nullptr && !buffered_->active()))) {
        mark_lost();
    }
}

HRESULT device_input::lose(std::uint64_t expected) {
    std::scoped_lock lock{gate_};
    if (generation_ == expected) {
        mark_lost();
        return loss_;
    }
    return acquisition_ == acquisition::lost ? loss_ : DIERR_INPUTLOST;
}

HRESULT device_input::configure_allowed() noexcept {
    if (!access_->open()) {
        return DIERR_INPUTLOST;
    }
    observe_acquisition_loss();
    return acquisition_ == acquisition::acquired ? DIERR_ACQUIRED : DI_OK;
}

HRESULT device_input::check_acquisition(const input_access::call& call) {
    observe_acquisition_loss();
    if (acquisition_ != acquisition::acquired) {
        return acquisition_error();
    }
    if (!window_matches(call, acquired_window_)) {
        mark_lost();
        return DIERR_INPUTLOST;
    }
    return DI_OK;
}
} // namespace inputnext::dinput
