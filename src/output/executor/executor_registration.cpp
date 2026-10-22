#include "executor_state.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::output {

execution_result<backend_handle> executor::implementation::register_backend(
    const connection_target target,
    std::unique_ptr<native_sink>& candidate,
    const channel_mask supported_channels,
    const std::optional<backend_handle> expected) {
    auto reservation = engine->reserve_backend(target, candidate, supported_channels, expected);
    if (!reservation.has_value()) {
        return std::unexpected{reservation.error()};
    }

    if (reservation->start_worker) {
        const auto started = start_delivery_worker(*reservation);
        if (!started.has_value()) {
            return std::unexpected{started.error()};
        }
    }

    auto committed = engine->commit_backend(*reservation, candidate);
    if (committed.has_value()) {
        return committed;
    }

    const auto canceled = engine->cancel_backend(*reservation);
    if (!canceled.has_value()) {
        close_admission();
    }
    if (engine->worker_should_exit(reservation->lane)) {
        bool accepting_operations = false;
        {
            std::scoped_lock lock{lifecycle_gate};
            accepting_operations = accepting;
        }
        if (accepting_operations) {
            const auto joined = stop_and_join_worker(reservation->lane);
            if (!joined.has_value()) {
                return std::unexpected{execution_error::resources};
            }
        }
    }
    if (!canceled.has_value()) {
        return std::unexpected{execution_error::resources};
    }
    return std::unexpected{committed.error()};
}

execution_result<backend_handle> executor::register_backend(const connection_target target,
    std::unique_ptr<native_sink>& candidate,
    const channel_mask supported_channels) {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->register_backend(target, candidate, supported_channels, std::nullopt);
}

execution_result<backend_handle> executor::replace_backend(const backend_handle expected,
    const connection_target target,
    std::unique_ptr<native_sink>& candidate,
    const channel_mask supported_channels) {
    const implementation::operation active{*implementation_};
    if (!active.entered()) {
        return std::unexpected{execution_error::not_running};
    }
    return implementation_->register_backend(target, candidate, supported_channels, expected);
}

} // namespace inputnext::output
