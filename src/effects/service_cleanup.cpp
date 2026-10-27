#include "implementation.hpp"

#include "error_mapping.hpp"

#include "effects/effects.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <mutex>
#include <new>

namespace inputnext::effects {
namespace {

void retain_failure(cleanup_report& report, const status failure) noexcept {
    if (!report.failure.has_value()) {
        report.failure = failure;
    }
}

template <typename State>
void maintain_retired_open(State& state, context_record& record, cleanup_report& report) noexcept {
    const auto closed = state.close_record(record);
    if (closed != status::ok) {
        record.cleanup_selected = true;
        retain_failure(report, closed);
        return;
    }

    const auto released = state.release_native(record);
    if (released != status::ok) {
        retain_failure(report, released);
    }
}

template <typename State>
void maintain_live_open(State& state, context_record& record, cleanup_report& report) noexcept {
    try {
        const service_dependencies& dependencies = state.dependencies;
        const auto inspected = dependencies.authority.inspect(record.native_context);
        if (inspected.has_value()) {
            record.cleanup_selected = false;
            return;
        }
        if (inspected.error() == output::error::revoked) {
            const auto closed = state.close_record(record);
            if (closed != status::ok) {
                retain_failure(report, closed);
            } else {
                record.cleanup_selected = false;
            }
            return;
        }
        if (inspected.error() == output::error::stale_context &&
            state.target_retired(record.target)) {
            State::retain_retired_close(record);
            return;
        }
        record.cleanup_selected = true;
        retain_failure(report, map_output_error(inspected.error()));
    } catch (const std::bad_alloc&) {
        record.cleanup_selected = true;
        retain_failure(report, status::capacity_exceeded);
    } catch (...) {
        record.cleanup_selected = true;
        retain_failure(report, status::io_failed);
    }
}

template <typename State>
void maintain_open(State& state, context_record& record, cleanup_report& report) noexcept {
    if (state.target_retired(record.target)) {
        maintain_retired_open(state, record, report);
        return;
    }
    maintain_live_open(state, record, report);
}

template <typename State>
void maintain_retained(State& state, context_record& record, cleanup_report& report) noexcept {
    if (record.native_released || !state.target_retired(record.target)) {
        return;
    }
    record.cleanup_selected = true;
    const auto released = state.release_native(record);
    if (released != status::ok) {
        retain_failure(report, released);
    }
}

template <typename State>
void maintain_record(State& state, context_record& record, cleanup_report& report) noexcept {
    if (record.phase == context_phase::open) {
        maintain_open(state, record, report);
        return;
    }
    if (record.phase == context_phase::closed_retained) {
        maintain_retained(state, record, report);
    }
}

} // namespace

cleanup_report service::maintain() noexcept {
    auto& state = *implementation_;
    std::scoped_lock lock{state.gate};
    cleanup_report report{.admission_closed = !state.accepting};

    for (auto& record : state.contexts) {
        maintain_record(state, record, report);
    }

    report.occupied_contexts = state.occupied_contexts();
    report.pending_native_cleanup = state.pending_cleanup();
    return report;
}

cleanup_report service::revoke_for_shutdown() noexcept {
    auto& state = *implementation_;
    std::scoped_lock lock{state.gate};
    state.accepting = false;
    cleanup_report report{.admission_closed = true};

    for (auto& record : state.contexts) {
        if (record.phase == context_phase::open) {
            record.cleanup_selected = true;
            const auto closed = state.close_record(record);
            if (closed != status::ok) {
                retain_failure(report, closed);
                continue;
            }
        }
        if (record.phase == context_phase::closed_retained && !record.native_released) {
            record.cleanup_selected = true;
            const auto released = state.release_native(record);
            if (released != status::ok) {
                retain_failure(report, released);
            }
        }
    }

    report.occupied_contexts = state.occupied_contexts();
    report.pending_native_cleanup = state.pending_cleanup();
    return report;
}

} // namespace inputnext::effects
