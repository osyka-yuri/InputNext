#include "lease_state.hpp"

#include "dinput/control_wait.hpp"
#include "dinput/native_types.hpp"
#include "lease.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace inputnext::dinput {
namespace {

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

void exclusive_lease::state::close_handle(const HANDLE value) noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
        static_cast<void>(CloseHandle(value));
    }
}

bool exclusive_lease::state::cancelled() const noexcept {
    std::scoped_lock lock{gate};
    return cancel_requested;
}

bool exclusive_lease::state::prepared() const noexcept {
    std::scoped_lock lock{gate};
    return prepared_ready;
}

bool exclusive_lease::state::is_closed() const noexcept {
    std::scoped_lock lock{gate};
    return current == phase::closed;
}

HRESULT exclusive_lease::state::preparation_result() const noexcept {
    std::scoped_lock lock{gate};
    return prepared_ready ? prepared_status : E_PENDING;
}

HRESULT exclusive_lease::state::close_result() const noexcept {
    std::scoped_lock lock{gate};
    return current == phase::closed ? close_status : E_PENDING;
}

bool exclusive_lease::state::is_live() const noexcept {
    std::scoped_lock lock{gate};
    return current == phase::active && !cancel_requested;
}

HRESULT exclusive_lease::state::start() noexcept {
    {
        std::scoped_lock lock{gate};
        if (cancel_requested || current == phase::closing || current == phase::closed) {
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
    if (cancel_requested || current != phase::prepared || FAILED(prepared_status)) {
        return false;
    }
    current = phase::active;
    return true;
}

void exclusive_lease::state::request_cancel() noexcept {
    {
        std::scoped_lock lock{gate};
        if (current == phase::closed) {
            return;
        }
        cancel_requested = true;
        current = phase::closing;
    }
    static_cast<void>(SetEvent(cancellation_event));
}

void exclusive_lease::state::publish_prepared(const HRESULT result) noexcept {
    {
        std::scoped_lock lock{gate};
        if (prepared_ready) {
            return;
        }
        prepared_ready = true;
        prepared_status = cancel_requested ? DIERR_INPUTLOST : result;
        current = SUCCEEDED(prepared_status) ? phase::prepared : phase::closing;
    }
    static_cast<void>(SetEvent(prepared_event));
}

void exclusive_lease::state::publish_closed(const HRESULT result) noexcept {
    {
        std::scoped_lock lock{gate};
        if (!prepared_ready) {
            prepared_ready = true;
            prepared_status = cancel_requested ? DIERR_INPUTLOST : result;
        }
        close_status = result;
        current = phase::closed;
    }
    static_cast<void>(SetEvent(prepared_event));
    static_cast<void>(SetEvent(closed_event));
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
    if (state_->cancelled()) {
        return DIERR_INPUTLOST;
    }
    if (state_->prepared()) {
        return state_->preparation_result();
    }

    const std::array events{state_->cancellation_event, state_->prepared_event};
    std::size_t signalled{};
    const HRESULT waited = wait_for_native_control(events, signalled);
    if (waited != DI_OK) {
        return waited;
    }
    return signalled == 0U ? DIERR_INPUTLOST : state_->preparation_result();
}

bool exclusive_lease::try_activate() noexcept {
    return state_->activate();
}

bool exclusive_lease::live() const noexcept {
    return state_->is_live();
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
