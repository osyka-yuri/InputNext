#include "lease_state.hpp"

#include "dinput/control_wait.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "lease.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::dinput {
namespace {

using std::chrono::steady_clock;
inline constexpr auto proof_lifetime = std::chrono::milliseconds{250};
inline constexpr auto preparation_budget = std::chrono::milliseconds{250};

[[nodiscard]] HRESULT last_error_result() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

} // namespace

exclusive_lease::state::~state() {
    close_handle(start_event);
    close_handle(prepared_event);
    close_handle(cancellation_event);
    close_handle(closed_event);
}

exclusive_lease::state::state(const clock_callback now,
    std::shared_ptr<const void> context) noexcept
    : clock(now),
      clock_context(std::move(context)) {
}

void exclusive_lease::state::close_handle(const HANDLE value) noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(value));
    }
}

bool exclusive_lease::state::cancelled() const noexcept {
    std::scoped_lock lock{gate};
    expire_locked();
    return terminal.has_value();
}

exclusive_lease::state::preparation_wait exclusive_lease::state::prepare_wait() const noexcept {
    std::scoped_lock lock{gate};
    expire_locked();
    preparation_wait snapshot{.result = terminal.has_value() ? terminal : preparation,
        .deadline = std::nullopt};
    if (!snapshot.result.has_value()) {
        if (ResetEvent(prepared_event) == FALSE) {
            snapshot.result = last_error_result();
        } else if (preparation_deadline.has_value()) {
            snapshot.deadline = real_deadline(*preparation_deadline);
        }
    }
    return snapshot;
}

bool exclusive_lease::state::is_closed() const noexcept {
    std::scoped_lock lock{gate};
    return current == phase::closed;
}

HRESULT exclusive_lease::state::close_result() const noexcept {
    std::scoped_lock lock{gate};
    return current == phase::closed ? close_status : E_PENDING;
}

bool exclusive_lease::state::is_live() const noexcept {
    std::scoped_lock lock{gate};
    return authority_locked() == DI_OK && current == phase::active;
}

HRESULT exclusive_lease::state::authority() const noexcept {
    std::scoped_lock lock{gate};
    return authority_locked();
}

steady_clock::time_point exclusive_lease::state::now() const noexcept {
    return clock == nullptr ? steady_clock::now() : clock(clock_context.get());
}

steady_clock::time_point exclusive_lease::state::authority_deadline() const noexcept {
    std::scoped_lock lock{gate};
    return real_deadline(proof_deadline.value_or(steady_clock::time_point{}));
}

steady_clock::time_point exclusive_lease::state::real_deadline(
    const steady_clock::time_point deadline) const noexcept {
    if (clock == nullptr) {
        return deadline;
    }
    const auto real_now = steady_clock::now();
    return real_now + (deadline - now());
}

HRESULT exclusive_lease::state::record_probe(const HRESULT result,
    const steady_clock::time_point started,
    const bool initial,
    const std::atomic<bool>& admission_closed,
    const input_access& access) noexcept {
    std::scoped_lock lock{gate};
    const HRESULT status = record_probe_locked(result, started, initial, admission_closed, access);
    if (initial && (status != E_PENDING || terminal.has_value())) {
        publish_prepared_locked(status);
    }
    return status;
}

HRESULT exclusive_lease::state::start() noexcept {
    {
        std::scoped_lock lock{gate};
        if (terminal.has_value() || current == phase::closing || current == phase::closed) {
            return DIERR_INPUTLOST;
        }
        if (current != phase::reserved) {
            return S_FALSE;
        }
        current = phase::started;
    }
    if (SetEvent(start_event) == FALSE) {
        const HRESULT failure = last_error_result();
        request_cancel();
        return failure;
    }
    return DI_OK;
}

bool exclusive_lease::state::activate() noexcept {
    std::scoped_lock lock{gate};
    if (authority_locked() != DI_OK || current != phase::prepared) {
        return false;
    }
    current = phase::active;
    return true;
}

void exclusive_lease::state::request_cancel() noexcept {
    std::scoped_lock lock{gate};
    cancel_locked(DIERR_INPUTLOST);
}

void exclusive_lease::state::publish_prepared(const HRESULT result) noexcept {
    std::scoped_lock lock{gate};
    publish_prepared_locked(result);
}

void exclusive_lease::state::publish_closed(const HRESULT result) noexcept {
    {
        std::scoped_lock lock{gate};
        close_status = result;
        current = phase::closed;
    }
    static_cast<void>(SetEvent(prepared_event));
    static_cast<void>(SetEvent(closed_event));
}

HRESULT exclusive_lease::state::record_probe_locked(const HRESULT result,
    const steady_clock::time_point started,
    const bool initial,
    const std::atomic<bool>& admission_closed,
    const input_access& access) noexcept {
    const auto completed = now();
    const auto deadline = started + proof_lifetime;
    if (terminal.has_value()) {
        return *terminal;
    }
    if (admission_closed.load(std::memory_order_acquire) || !access.open()) {
        cancel_locked(DIERR_INPUTLOST);
        return DIERR_INPUTLOST;
    }
    expire_locked();
    if (terminal.has_value()) {
        return *terminal;
    }
    if ((!initial && current != phase::prepared && current != phase::active) ||
        (proof_deadline.has_value() && completed >= *proof_deadline) ||
        (result == DI_OK && completed >= deadline)) {
        cancel_locked(DIERR_INPUTLOST);
        return DIERR_INPUTLOST;
    }
    if (result == DI_OK) {
        proof_deadline = deadline;
        return DI_OK;
    }
    if (initial && result == E_PENDING) {
        if (!preparation_deadline.has_value()) {
            preparation_deadline = started + preparation_budget;
            static_cast<void>(SetEvent(prepared_event));
        }
        expire_locked();
        return terminal.value_or(E_PENDING);
    }
    if (result == DIERR_NOTACQUIRED) {
        cancel_locked(result);
        return result;
    }
    if (initial || result == E_PENDING) {
        return FAILED(result) ? result : DIERR_INPUTLOST;
    }
    cancel_locked(DIERR_INPUTLOST);
    return DIERR_INPUTLOST;
}

void exclusive_lease::state::publish_prepared_locked(const HRESULT result) noexcept {
    if (preparation.has_value()) {
        return;
    }
    preparation = terminal.value_or(result);
    preparation_deadline.reset();
    current = SUCCEEDED(*preparation) ? phase::prepared : phase::closing;
    static_cast<void>(SetEvent(prepared_event));
}

HRESULT exclusive_lease::state::authority_locked() const noexcept {
    expire_locked();
    if (terminal.has_value()) {
        return *terminal == E_PENDING ? DIERR_INPUTLOST : *terminal;
    }
    return current == phase::prepared || current == phase::active ? DI_OK : DIERR_INPUTLOST;
}

void exclusive_lease::state::expire_locked() const noexcept {
    if (!terminal.has_value() && proof_deadline.has_value() && now() >= *proof_deadline) {
        cancel_locked(DIERR_INPUTLOST);
    }
    if (!terminal.has_value() && preparation_deadline.has_value() &&
        now() >= *preparation_deadline) {
        cancel_locked(E_PENDING);
    }
}

void exclusive_lease::state::cancel_locked(const HRESULT reason) const noexcept {
    if (terminal.has_value() || current == phase::closed) {
        return;
    }
    terminal = reason;
    current = phase::closing;
    static_cast<void>(SetEvent(cancellation_event));
}

exclusive_lease::exclusive_lease(std::shared_ptr<state> state_value) noexcept
    : state_(std::move(state_value)) {
}

exclusive_lease::~exclusive_lease() {
    cancel();
}

HRESULT exclusive_lease::start() noexcept {
    return state_->start();
}

HRESULT exclusive_lease::wait_prepared() noexcept {
    const std::array events{state_->cancellation_event, state_->prepared_event};
    for (;;) {
        const auto snapshot = state_->prepare_wait();
        if (snapshot.result.has_value()) {
            return *snapshot.result;
        }
        std::size_t signalled{};
        const HRESULT waited = wait_for_native_control(events, signalled, snapshot.deadline);
        if (waited != DI_OK && waited != HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
            return waited;
        }
    }
}

bool exclusive_lease::try_activate() noexcept {
    return state_->activate();
}

bool exclusive_lease::live() const noexcept {
    return state_->is_live();
}

HRESULT exclusive_lease::authority_status() const noexcept {
    return state_->authority();
}

steady_clock::time_point exclusive_lease::authority_deadline() const noexcept {
    return state_->authority_deadline();
}

void exclusive_lease::cancel() noexcept {
    state_->request_cancel();
}

bool exclusive_lease::closed() const noexcept {
    return state_->is_closed();
}

HRESULT exclusive_lease::wait_closed() noexcept {
    if (closed()) {
        return state_->close_result();
    }
    const std::array events{state_->closed_event};
    std::size_t signalled{};
    const HRESULT waited = wait_for_native_control(events, signalled);
    if (waited != DI_OK) {
        return waited;
    }
    return state_->close_result();
}

HANDLE exclusive_lease::cancellation_event() const noexcept {
    return state_->cancellation_event;
}

} // namespace inputnext::dinput
