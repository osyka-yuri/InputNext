#include "engine_state.hpp"

#include "drive_policy.hpp"
#include "engine.hpp"
#include "output/authority/execution.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>

namespace inputnext::output::detail {

execution_engine::implementation::drive_action execution_engine::implementation::prepare_drive(
    const lane_token token) noexcept {
    std::scoped_lock lock{gate};
    const auto* const lane = find_lane(token);
    if (lane == nullptr || lane_should_exit(token)) {
        return {};
    }

    if (auto* const retiring = find_retiring(token); retiring != nullptr) {
        drive_action action{};
        action.backend = retiring->handle;
        action.cleanup_sink = std::move(retiring->sink);
        if (action.cleanup_sink == nullptr) {
            retiring->state = backend_state::drained;
            notify_lane(token);
            action.kind = drive_action_kind::progressed;
            return action;
        }
        retiring->state = backend_state::cleaning;
        action.kind = drive_action_kind::cleanup;
        return action;
    }

    auto* const backend = find_current(token);
    if (backend == nullptr || !backend->delivery_needed || backend->refresh_requested ||
        backend->write_in_flight || backend->due > clock::now()) {
        return {};
    }
    if (!backend->projection.has_value() || backend->next_attempt_id == 0U) {
        backend->delivery = delivery_state::identity_exhausted;
        backend->delivery_needed = false;
        retire_locked(*backend, retirement_reason::identity_exhausted);
        return {.kind = drive_action_kind::progressed};
    }

    const auto qualifies =
        authority_execution_access::qualifies(owner, execution, *backend->projection);
    if (!apply_qualification_result(*backend, qualifies)) {
        return {.kind = drive_action_kind::progressed};
    }

    drive_action action{};
    action.kind = drive_action_kind::write;
    action.backend = backend->handle;
    const auto attempt = attempt_id{backend->next_attempt_id};
    if (backend->next_attempt_id == std::numeric_limits<std::uint64_t>::max()) {
        backend->next_attempt_id = 0U;
    } else {
        ++backend->next_attempt_id;
    }
    action.command = native_command{.backend = action.backend,
        .publication = backend->publication,
        .attempt = attempt,
        .output = native_values(*backend->projection)};
    action.sink = backend->sink.get();

    backend->last_attempt = attempt;
    backend->write_in_flight = true;
    backend->delivery = delivery_state::in_flight;
    backend->retry_requested = false;
    return action;
}

void execution_engine::implementation::finish_cleanup(const lane_token token,
    const backend_handle handle) noexcept {
    std::scoped_lock lock{gate};
    auto* const backend = find_backend(handle);
    if (backend == nullptr || backend->lane != token || backend->state != backend_state::cleaning ||
        backend->sink != nullptr) {
        return;
    }
    backend->state = backend_state::drained;
    notify_lane(token);
}

void execution_engine::implementation::apply_native_result(backend_record& backend,
    const native_result result) noexcept {
    switch (result.status) {
    case native_status::accepted:
        backend.consecutive_retryable_failures = 0U;
        backend.delivery = delivery_state::setter_accepted;
        backend.delivery_needed = false;
        break;
    case native_status::retryable_failure:
        backend.consecutive_retryable_failures =
            drive_policy::next_retryable_failure_count(backend.consecutive_retryable_failures);
        backend.delivery = delivery_state::retry_wait;
        backend.delivery_needed = true;
        backend.due = drive_policy::deadline_after(clock::now(),
            drive_policy::retry_delay_for(backend.consecutive_retryable_failures));
        break;
    case native_status::unsupported:
    case native_status::failed:
        backend.consecutive_retryable_failures = 0U;
        backend.delivery = delivery_state::blocked;
        backend.delivery_needed = false;
        break;
    case native_status::disconnected:
        retire_locked(backend, retirement_reason::disconnected);
        break;
    }
}

bool execution_engine::implementation::apply_qualification_result(backend_record& backend,
    const execution_result<bool>& result) noexcept {
    if (result.has_value() && *result) {
        return true;
    }
    if (!result.has_value() && result.error() == execution_error::not_connected) {
        retire_locked(backend, retirement_reason::connection_retired);
        return false;
    }
    if (!result.has_value() && result.error() == execution_error::stale_backend) {
        retire_locked(backend, retirement_reason::shutdown);
        return false;
    }

    backend.delivery = delivery_state::pending;
    backend.delivery_needed = false;
    backend.refresh_requested = true;
    notify_coordinator_refresh();
    notify_lane(backend.lane);
    return false;
}

void execution_engine::implementation::finish_attempt(const native_command& command,
    const native_result result) noexcept {
    std::scoped_lock lock{gate};
    auto* const backend = find_backend(command.backend);
    if (backend == nullptr || backend->last_attempt != command.attempt ||
        !backend->write_in_flight) {
        return;
    }
    backend->write_in_flight = false;
    backend->last_completed_attempt = completed_attempt{.id = command.attempt,
        .publication = command.publication,
        .result = result,
        .output = command.output};

    if (backend->state != backend_state::current) {
        notify_lane(backend->lane);
        return;
    }
    if (result.status == native_status::disconnected) {
        // Disconnection applies to this backend even when the completed publication is stale.
        apply_native_result(*backend, result);
        return;
    }
    if (backend->publication != command.publication) {
        notify_lane(backend->lane);
        return;
    }
    if (!backend->projection.has_value()) {
        backend->delivery = delivery_state::pending;
        backend->delivery_needed = false;
        backend->refresh_requested = true;
        notify_coordinator_refresh();
        notify_lane(backend->lane);
        return;
    }

    const auto still_current =
        authority_execution_access::qualifies(owner, execution, *backend->projection);
    if (!apply_qualification_result(*backend, still_current)) {
        return;
    }

    apply_native_result(*backend, result);
    if (backend->state == backend_state::current && backend->next_attempt_id == 0U) {
        backend->delivery = delivery_state::identity_exhausted;
        backend->delivery_needed = false;
        retire_locked(*backend, retirement_reason::identity_exhausted);
        return;
    }
    if (backend->retry_requested) {
        backend->delivery = delivery_state::pending;
        backend->delivery_needed = true;
        backend->due = clock::now();
    }
    notify_lane(backend->lane);
}

bool execution_engine::wait_for_work(const lane_token token, const std::stop_token& stop_token) {
    std::unique_lock lock{implementation_->gate};
    auto* lane = implementation_->find_lane(token);
    if (lane == nullptr || lane->phase == implementation::lane_phase::free) {
        return false;
    }

    for (;;) {
        lane = implementation_->find_lane(token);
        if (lane == nullptr || lane->phase == implementation::lane_phase::free) {
            return false;
        }
        if (implementation_->lane_has_pending_cleanup(token)) {
            return true;
        }
        if (stop_token.stop_requested()) {
            return false;
        }
        if (implementation_->lane_has_due_work(token, clock::now())) {
            return true;
        }
        if (implementation_->lane_should_exit(token)) {
            return false;
        }

        const auto next = implementation_->next_due(token);
        const bool waiting_for_start = lane->phase == implementation::lane_phase::starting;
        const auto ready = [this, token, &stop_token, waiting_for_start] {
            const auto* const current_lane = implementation_->find_lane(token);
            return stop_token.stop_requested() || current_lane == nullptr ||
                   (waiting_for_start &&
                       current_lane->phase != implementation::lane_phase::starting) ||
                   implementation_->lane_should_exit(token) ||
                   implementation_->lane_has_due_work(token, clock::now());
        };
        if (next.has_value()) {
            lane->changed.wait_until(lock, stop_token, *next, ready);
        } else {
            lane->changed.wait(lock, stop_token, ready);
        }
    }
    return false;
}

bool execution_engine::drive_one(const lane_token token) noexcept {
    implementation::lane_record* lane = nullptr;
    {
        std::scoped_lock lock{implementation_->gate};
        lane = implementation_->find_lane(token);
        if (lane == nullptr || lane->phase == implementation::lane_phase::starting ||
            lane->phase == implementation::lane_phase::free) {
            return false;
        }
    }

    std::scoped_lock physical_lock{lane->physical_fence};
    auto action = implementation_->prepare_drive(token);
    switch (action.kind) {
    case implementation::drive_action_kind::idle:
        return false;
    case implementation::drive_action_kind::progressed:
        return true;
    case implementation::drive_action_kind::cleanup:
        action.cleanup_sink.reset();
        implementation_->finish_cleanup(token, action.backend);
        return true;
    case implementation::drive_action_kind::write: {
        const auto result = action.sink->write(action.command);
        implementation_->finish_attempt(action.command, result);
        return true;
    }
    }
    return false;
}

execution_result<void> execution_engine::request_retry(const backend_handle handle) noexcept {
    std::scoped_lock lock{implementation_->gate};
    auto* const backend = implementation_->find_backend(handle);
    if (backend != nullptr && backend->delivery == delivery_state::identity_exhausted) {
        return std::unexpected{execution_error::identity_exhausted};
    }
    if (backend == nullptr || backend->state != implementation::backend_state::current) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (backend->next_attempt_id == 0U || backend->next_publication_id == 0U ||
        backend->delivery == delivery_state::identity_exhausted) {
        return std::unexpected{execution_error::identity_exhausted};
    }
    backend->retry_requested = true;
    backend->delivery_needed = true;
    backend->delivery = delivery_state::pending;
    backend->due = clock::now();
    implementation_->notify_lane(backend->lane);
    return {};
}

} // namespace inputnext::output::detail
