#include "engine_state.hpp"

#include "drive_policy.hpp"
#include "engine.hpp"
#include <inputnext/output_execution.hpp>

#include <expected>
#include <mutex>
#include <stop_token>

namespace inputnext::output::detail {

bool execution_engine::wait_for_refresh(const std::stop_token& stop_token) {
    auto& state = *implementation_;
    std::unique_lock lock{state.gate};
    const auto ready = [&state, &stop_token] {
        return stop_token.stop_requested() || state.closed || state.coordinator_refresh_requested;
    };

    if (state.current_backend_count() == 0U) {
        const bool signaled = state.refresh_changed.wait(lock, stop_token, ready);
        if (!signaled) {
            return false;
        }
    } else {
        const bool signaled =
            state.refresh_changed.wait_for(lock, stop_token, drive_policy::refresh_interval, ready);
        if (!signaled && state.current_backend_count() == 0U) {
            return false;
        }
    }

    if (stop_token.stop_requested() || state.closed) {
        return false;
    }
    state.coordinator_refresh_requested = false;
    return true;
}

execution_result<current_backend_batch> execution_engine::current_backends() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    current_backend_batch batch{};
    for (const auto& backend : implementation_->backends) {
        if (backend.state != implementation::backend_state::current) {
            continue;
        }
        if (batch.count == batch.handles.size()) {
            return std::unexpected{execution_error::capacity_exceeded};
        }
        batch.handles[batch.count++] = backend.handle;
    }
    return batch;
}

bool execution_engine::has_current_backends() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return implementation_->current_backend_count() != 0U;
}

void execution_engine::request_drive() noexcept {
    std::scoped_lock lock{implementation_->gate};
    if (!implementation_->closed) {
        implementation_->notify_coordinator_refresh();
    }
}

} // namespace inputnext::output::detail
