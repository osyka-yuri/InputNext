#include "engine_state.hpp"

#include "engine.hpp"
#include "output/body/pcm_collection.hpp"
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <expected>
#include <mutex>

namespace inputnext::output::detail {

execution_result<retirement_receipt> execution_engine::retire(const backend_handle handle,
    const retirement_reason reason) noexcept {
    const body_pcm_collection_on_exit collect{implementation_->owner};
    std::scoped_lock lock{implementation_->gate};
    auto* const backend = implementation_->find_backend(handle);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!backend->receipt.has_value()) {
        implementation_->retire_locked(*backend, reason);
    }
    if (!backend->receipt.has_value()) {
        return std::unexpected{execution_error::resources};
    }
    return *backend->receipt;
}

execution_result<void> execution_engine::acknowledge_retirement(
    const retirement_receipt& receipt) noexcept {
    std::scoped_lock lock{implementation_->gate};
    auto* const backend = implementation_->find_backend(receipt.backend);
    if (backend == nullptr || !backend->receipt.has_value() || *backend->receipt != receipt) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (backend->state != implementation::backend_state::drained) {
        return std::unexpected{execution_error::pending};
    }
    const auto lane = backend->lane;
    *backend = implementation::backend_record{};
    implementation_->maybe_close_lane_locked(lane);
    implementation_->notify_lane(lane);
    return {};
}

void execution_engine::close_all() noexcept {
    const body_pcm_collection_on_exit collect{implementation_->owner};
    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    state.closed = true;
    state.refresh_changed.notify_all();
    for (std::size_t index = 0U; index < state.lanes.size(); ++index) {
        auto& lane = state.lanes[index];
        if (lane.phase == implementation::lane_phase::free) {
            continue;
        }
        lane.shutdown_requested = true;
        for (auto& backend : state.backends) {
            if (backend.state == implementation::backend_state::current &&
                backend.lane == lane.token) {
                state.retire_locked(backend, retirement_reason::shutdown);
            }
        }
        state.notify_lane(lane.token);
    }
}

bool execution_engine::worker_should_exit(const lane_token token) const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return implementation_->lane_should_exit(token);
}

execution_result<void> execution_engine::worker_joined(const lane_token token) noexcept {
    const body_pcm_collection_on_exit collect{implementation_->owner};
    std::scoped_lock lock{implementation_->gate};
    auto* const lane = implementation_->find_lane(token);
    if (lane == nullptr || !implementation_->lane_should_exit(token)) {
        return std::unexpected{execution_error::busy};
    }

    if (lane->shutdown_requested) {
        for (auto& backend : implementation_->backends) {
            if (backend.lane != token || backend.state == implementation::backend_state::free) {
                continue;
            }
            if (backend.state != implementation::backend_state::drained ||
                backend.sink != nullptr) {
                return std::unexpected{execution_error::pending};
            }
            backend = implementation::backend_record{};
        }
    } else if (implementation_->lane_has_records(token) ||
               implementation_->lane_has_reservations(token)) {
        return std::unexpected{execution_error::pending};
    }

    lane->phase = implementation::lane_phase::free;
    lane->token = {};
    lane->runtime = {};
    lane->device = {};
    lane->shutdown_requested = false;
    return {};
}

} // namespace inputnext::output::detail
