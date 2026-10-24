#include "state.hpp"

#include "backend_selection.hpp"
#include "manager.hpp"
#include "schedule.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>

namespace inputnext::gameinput_detail {
namespace {

constexpr std::array retry_delays{std::chrono::milliseconds{4},
    std::chrono::milliseconds{8},
    std::chrono::milliseconds{16},
    std::chrono::milliseconds{32},
    std::chrono::milliseconds{64}};

[[nodiscard]] bool same_device(const connection_target left,
    const connection_target right) noexcept {
    return left.runtime == right.runtime && left.device == right.device;
}

} // namespace

bool gameinput_output_connections::implementation::any_backend_locked() const noexcept {
    return std::ranges::any_of(backends, [](const auto& record) {
        return record.state != backend_state::free && record.handle.has_value();
    });
}

bool gameinput_output_connections::implementation::has_usb_route_locked(
    const connection_target target) const noexcept {
    return std::ranges::any_of(backends, [target](const auto& backend) {
        return backend.target == target && backend.kind == output_backend_kind::dualsense_usb &&
               backend.state != backend_state::free &&
               backend.state != backend_state::terminal_unresolved;
    });
}

connection_schedule::creation_state
gameinput_output_connections::implementation::creation_state_locked(
    const desired_record& entry) const noexcept {
    return connection_schedule::creation_state{.accepting =
                                                   phase == connection_control_phase::accepting,
        .fallback = fallback,
        .admissions_frozen = admissions_frozen,
        .occupied = entry.occupied,
        .description_known = entry.description_known,
        .has_output_backend =
            entry.selection.has_value() && entry.selection != output_backend_kind::none,
        .retry_blocked = entry.retry_blocked};
}

bool gameinput_output_connections::implementation::creation_allowed_locked(
    const desired_record& entry,
    const clock::time_point now) const noexcept {
    const auto state = creation_state_locked(entry);
    return connection_schedule::can_attempt_creation(state) &&
           (!entry.retry_armed || now >= entry.retry_due) &&
           (!entry.resource_waiting || now >= entry.resource_due);
}

bool gameinput_output_connections::implementation::has_same_device_predecessor_locked(
    const connection_target target) const noexcept {
    return std::ranges::any_of(backends, [&](const auto& record) {
        return record.state != backend_state::free && record.target.valid() &&
               record.target != target && same_device(record.target, target);
    });
}

gameinput_output_connections::implementation::desired_record*
gameinput_output_connections::implementation::find_desired_locked(
    const connection_target target) noexcept {
    const auto found = std::ranges::find_if(desired,
        [&](const auto& entry) { return entry.occupied && entry.target == target; });
    return found == desired.end() ? nullptr : &*found;
}

const gameinput_output_connections::implementation::desired_record*
gameinput_output_connections::implementation::find_desired_locked(
    const connection_target target) const noexcept {
    const auto found = std::ranges::find_if(desired,
        [&](const auto& entry) { return entry.occupied && entry.target == target; });
    return found == desired.end() ? nullptr : &*found;
}

std::size_t
gameinput_output_connections::implementation::find_free_backend_locked() const noexcept {
    const auto found = std::ranges::find_if(backends,
        [](const auto& entry) { return entry.state == backend_state::free; });
    return static_cast<std::size_t>(found - backends.begin());
}

connection_control_report
gameinput_output_connections::implementation::report_locked() const noexcept {
    const auto quiescent = creation_handoffs == 0U && candidate_obligations == 0U;
    return connection_control_report{.phase = phase,
        .admission_quiescent = quiescent,
        .control_finished = control_finished,
        .control_joined = control_joined,
        .all_retirements_acknowledged = all_retirements_acknowledged && backend_obligations == 0U &&
                                        candidate_obligations == 0U && !fallback,
        .executor_shutdown_returned = executor_shutdown_returned,
        .candidate_obligations = candidate_obligations,
        .backend_obligations = backend_obligations,
        .pending_retirements = pending_retirements,
        .failure = first_failure};
}

void gameinput_output_connections::implementation::fail_locked(
    connection_control_failure failure) noexcept {
    if (!first_failure.has_value()) {
        first_failure = failure;
    }
}

void gameinput_output_connections::implementation::fallback_locked(
    connection_control_failure failure) noexcept {
    fail_locked(failure);
    for (auto const& entry : desired) {
        if (entry.qualification != nullptr) {
            entry.qualification->revoke();
        }
    }
    for (auto const& backend : backends) {
        if (backend.qualification != nullptr) {
            backend.qualification->revoke();
        }
    }
    fallback = true;
    admissions_frozen = true;
    phase = connection_control_phase::needs_executor_shutdown;
    all_retirements_acknowledged = false;
    for (auto& record : backends) {
        if (record.handle.has_value()) {
            record.state = backend_state::terminal_unresolved;
        }
    }
    changed.notify_all();
}

void gameinput_output_connections::implementation::arm_retry_locked(desired_record& entry,
    const clock::time_point completed) noexcept {
    const auto delay_index = std::min<std::size_t>(entry.retry_level, retry_delays.size() - 1U);
    entry.retry_due = completed + retry_delays[delay_index];
    entry.retry_armed = true;
    entry.retry_blocked = false;
    if (entry.retry_level < retry_delays.size() - 1U) {
        ++entry.retry_level;
    }
}

void gameinput_output_connections::implementation::reset_retry_locked(
    desired_record& entry) noexcept {
    entry.retry_level = 0U;
    entry.retry_armed = false;
    entry.retry_due = clock::time_point{};
    entry.retry_blocked = false;
    entry.resource_waiting = false;
    entry.resource_due = clock::time_point{};
}

void gameinput_output_connections::implementation::complete_candidate_locked(
    const std::size_t slot) noexcept {
    if (candidate_obligations > 0U) {
        --candidate_obligations;
    }
    if (creation_handoffs > 0U) {
        --creation_handoffs;
    }
    if (slot < backends.size() && (backends[slot].state == backend_state::creating ||
                                      backends[slot].state == backend_state::registering)) {
        backends[slot] = backend_record{};
    }
    admission_quiescent = creation_handoffs == 0U && candidate_obligations == 0U;
    changed.notify_all();
}

void gameinput_output_connections::implementation::finish_worker_locked() noexcept {
    control_finished = true;
    phase = connection_control_phase::needs_executor_shutdown;
    admission_quiescent = creation_handoffs == 0U && candidate_obligations == 0U;
    changed.notify_all();
}

void gameinput_output_connections::implementation::publish_backend_progress_locked(
    const std::size_t slot,
    output::backend_progress value) noexcept {
    if (slot < backends.size() && backends[slot].handle == value.backend) {
        backends[slot].cached_progress = value;
    }
}

bool gameinput_output_connections::implementation::should_retire_locked(
    const backend_record& backend) const noexcept {
    if (phase == connection_control_phase::stopping) {
        return true;
    }
    const auto* const entry = find_desired_locked(backend.target);
    if (entry == nullptr) {
        return true;
    }
    if (entry->description_known && entry->selection != backend.kind) {
        return true;
    }
    if (backend.kind == output_backend_kind::dualsense_usb) {
        return backend.qualification == nullptr || backend.qualification != entry->qualification ||
               backend.qualification->target() != backend.target ||
               backend.qualification->catalogue_generation() != entry->catalogue_generation ||
               !backend.qualification->admits(backend.target, clock::now());
    }
    return false;
}

std::optional<gameinput_output_connections::implementation::clock::time_point>
gameinput_output_connections::implementation::next_deadline_locked() const noexcept {
    if (fallback || control_finished ||
        phase == connection_control_phase::needs_executor_shutdown ||
        phase == connection_control_phase::stopped) {
        return std::nullopt;
    }

    std::optional<clock::time_point> nearest;
    const auto include = [&](const clock::time_point value) {
        if (!nearest.has_value() || value < *nearest) {
            nearest = value;
        }
    };

    for (const auto& entry : desired) {
        if (!entry.occupied) {
            continue;
        }
        if (phase == connection_control_phase::accepting) {
            include(entry.next_description);
            const auto state = creation_state_locked(entry);
            const connection_schedule::retry_state retry{.retry_armed = entry.retry_armed,
                .retry_due = entry.retry_due,
                .resource_waiting = entry.resource_waiting,
                .resource_due = entry.resource_due};
            connection_schedule::include_creation_deadline(nearest, state, retry);
        }
        const bool live_usb_route = has_usb_route_locked(entry.target);
        if (needs_usb_qualification(entry.model) &&
            (phase == connection_control_phase::accepting ||
                (phase == connection_control_phase::admission_closed && live_usb_route))) {
            include(entry.next_qualification);
        }
    }
    if (any_backend_locked() && next_progress_scan.has_value()) {
        include(*next_progress_scan);
    }
    return nearest;
}

void gameinput_output_connections::implementation::schedule_after_cycle() noexcept {
    const auto completed = clock::now();
    std::scoped_lock lock{gate};
    if (any_backend_locked() && !fallback) {
        next_progress_scan = completed + std::chrono::milliseconds{4};
    } else {
        next_progress_scan = std::nullopt;
    }
    changed.notify_all();
}

void gameinput_output_connections::implementation::run_cycle() noexcept {
    try {
        scan_backends();
        {
            std::scoped_lock lock{gate};
            if (fallback) {
                return;
            }
        }
        scan_descriptions();
        scan_qualifications();
        attempt_one_creation();
        schedule_after_cycle();
    } catch (...) {
        std::scoped_lock lock{gate};
        fallback_locked(
            connection_control_failure{.operation = connection_control_operation::worker_exception,
                .target = {},
                .backend = std::nullopt,
                .runtime_failure = std::nullopt,
                .native_failure = std::nullopt,
                .executor_failure = std::nullopt});
    }
}

bool gameinput_output_connections::implementation::worker_work_ready_locked() const noexcept {
    const auto deadline = next_deadline_locked();
    return inventory_dirty || admission_dirty || stop_dirty || progress_dirty || control_finished ||
           worker_can_finish_locked() || (deadline.has_value() && *deadline <= clock::now());
}

bool gameinput_output_connections::implementation::worker_can_finish_locked() const noexcept {
    const bool no_candidate = candidate_obligations == 0U && creation_handoffs == 0U;
    return (phase == connection_control_phase::needs_executor_shutdown && no_candidate) ||
           (phase == connection_control_phase::stopping && no_candidate &&
               backend_obligations == 0U);
}

void gameinput_output_connections::implementation::worker_loop() noexcept {
    for (;;) {
        try {
            if (worker_iteration()) {
                return;
            }
        } catch (...) {
            handle_worker_exception();
            return;
        }
    }
}

void gameinput_output_connections::implementation::handle_worker_exception() noexcept {
    std::scoped_lock lock{gate};
    fallback_locked(
        connection_control_failure{.operation = connection_control_operation::worker_exception,
            .target = {},
            .backend = std::nullopt,
            .runtime_failure = std::nullopt,
            .native_failure = std::nullopt,
            .executor_failure = std::nullopt});
    finish_worker_locked();
}

bool gameinput_output_connections::implementation::worker_iteration() {
    bool close_runtime = false;
    {
        std::unique_lock lock{gate};
        while (!worker_work_ready_locked()) {
            const auto deadline = next_deadline_locked();
            if (deadline.has_value()) {
                static_cast<void>(changed.wait_until(lock, *deadline, [this] {
                    return worker_work_ready_locked();
                }));
            } else {
                changed.wait(lock, [this] { return worker_work_ready_locked(); });
            }
        }

        if (!runtime_admission_closed && phase != connection_control_phase::accepting) {
            runtime_admission_closed = true;
            close_runtime = true;
        }

        if (worker_can_finish_locked()) {
            lock.unlock();
            if (close_runtime) {
                runtime.close_admission();
            }
            std::scoped_lock finished_lock{gate};
            finish_worker_locked();
            return true;
        }

        inventory_dirty = false;
        admission_dirty = false;
        stop_dirty = false;
        progress_dirty = false;
    }
    if (close_runtime) {
        runtime.close_admission();
    }
    run_cycle();
    return false;
}

} // namespace inputnext::gameinput_detail
