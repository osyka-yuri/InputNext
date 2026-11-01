#include "gameinput_probe/output/rumble/report.hpp"

#include "gameinput/output/connections/manager.hpp"
#include "gameinput_probe/output/rumble/evidence.hpp"
#include "gameinput_probe/output/session.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output_execution.hpp>

#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] const char* focus_name(const focus::evidence value) noexcept {
    switch (value) {
    case focus::evidence::unavailable:
        return "unavailable";
    case focus::evidence::background:
        return "background";
    case focus::evidence::foreground:
        return "foreground";
    }
    return "unknown";
}

[[nodiscard]] const char* evidence_name(const rumble_native_evidence value) noexcept {
    switch (value) {
    case rumble_native_evidence::not_requested:
        return "not_requested";
    case rumble_native_evidence::pending:
        return "pending";
    case rumble_native_evidence::accepted_newer_attempt:
        return "setter_accepted_newer_matching_attempt";
    case rumble_native_evidence::historical_reuse:
        return "historical_reuse";
    case rumble_native_evidence::rejected:
        return "rejected";
    }
    return "unknown";
}

[[nodiscard]] const char* native_status_name(const output::native_status value) noexcept {
    switch (value) {
    case output::native_status::accepted:
        return "accepted";
    case output::native_status::retryable_failure:
        return "retryable_failure";
    case output::native_status::unsupported:
        return "unsupported";
    case output::native_status::failed:
        return "failed";
    case output::native_status::disconnected:
        return "disconnected";
    }
    return "unknown";
}

template <typename Bytes>
void append_hex_bytes(std::ostream& output, const Bytes& bytes) {
    for (const auto byte : bytes) {
        output << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned int>(byte);
    }
    output << std::dec << std::setfill(' ');
}

[[nodiscard]] const char* connection_phase_name(const connection_control_phase value) noexcept {
    switch (value) {
    case connection_control_phase::accepting:
        return "accepting";
    case connection_control_phase::admission_closed:
        return "admission_closed";
    case connection_control_phase::stopping:
        return "stopping";
    case connection_control_phase::needs_executor_shutdown:
        return "needs_executor_shutdown";
    case connection_control_phase::stopped:
        return "stopped";
    }
    return "unknown";
}

[[nodiscard]] const char* operation_name(const connection_control_operation value) noexcept {
    switch (value) {
    case connection_control_operation::describe:
        return "describe";
    case connection_control_operation::qualify_backend:
        return "qualify_backend";
    case connection_control_operation::reserve:
        return "reserve";
    case connection_control_operation::create_sink:
        return "create_sink";
    case connection_control_operation::register_backend:
        return "register_backend";
    case connection_control_operation::progress_backend:
        return "progress_backend";
    case connection_control_operation::retire_backend:
        return "retire_backend";
    case connection_control_operation::acknowledge_retirement:
        return "acknowledge_retirement";
    case connection_control_operation::worker_exception:
        return "worker_exception";
    }
    return "unknown";
}

} // namespace

bool rumble_readiness::supported() const noexcept {
    return assignment.has_value() && configured && (low_motor || high_motor);
}

bool rumble_readiness::ready() const noexcept {
    return supported() && foreground && window_foreground;
}

std::string format_rumble_target(const connection_target target) {
    std::ostringstream output;
    output << "runtime=";
    append_hex_bytes(output, target.runtime.bytes);
    output << " key=" << target.device.value << " connection=" << target.connection.value;
    return output.str();
}

std::string format_rumble_readiness(const rumble_readiness& state,
    const focus::snapshot& focus_snapshot) {
    std::ostringstream output;
    output << "rumble_ui visible=true window_foreground=" << std::boolalpha
           << state.window_foreground << " focus=" << focus_name(focus_snapshot.state)
           << " focus_revision=" << focus_snapshot.revision
           << " start=waiting_for_user ready=" << state.ready();
    if (state.assignment.has_value()) {
        output << " user=" << state.assignment->user_index << " assignment=" << state.assignment->id
               << ' ' << format_rumble_target(state.assignment->target);
    } else {
        output << " user=unassigned";
    }
    output << " output_configured=" << state.configured << " low_cap=" << state.low_motor
           << " high_cap=" << state.high_motor;
    return output.str();
}

std::string format_rumble_step(const rumble_step_report& report) {
    std::ostringstream output;
    output << std::boolalpha << "rumble_step name=" << report.step << ' '
           << format_rumble_target(report.target) << " assignment=" << report.assignment_id
           << " request_revision=" << report.request_revision
           << " logical_version=" << report.logical_version << " motors=" << report.left_motor
           << ',' << report.right_motor
           << " logical_channel_present=" << report.logical_channel_present
           << " logical_channel_version=";
    if (report.logical_channel_version.has_value()) {
        output << report.logical_channel_version.value();
    } else {
        output << "unavailable";
    }
    output << " logical_call=" << report.logical_call_made << " logical_result=";
    if (report.logical_result.has_value()) {
        output << "0x" << std::hex << report.logical_result.value() << std::dec;
    } else {
        output << "void_call";
    }
    output << " logical_state=" << report.logical_state_confirmed
           << " assignment_live=" << report.assignment_live << " foreground=" << report.foreground
           << " low_cap=" << report.low_motor_available
           << " high_cap=" << report.high_motor_available
           << " capabilities=" << report.capabilities_confirmed
           << " native_evidence=" << evidence_name(report.native_evidence);
    if (report.native_match.has_value()) {
        const auto& match = report.native_match.value();
        output << " backend_authority=";
        append_hex_bytes(output, match.backend.incarnation.owner.authority.bytes);
        output << " backend_execution=" << match.backend.incarnation.owner.id.value
               << " backend=" << match.backend.incarnation.id.value
               << " publication=" << match.publication.value << " attempt=" << match.attempt.value
               << " native_status=" << native_status_name(match.result.status)
               << " native_code=" << match.result.native_code
               << " native_channels=" << static_cast<unsigned int>(match.output.channels)
               << " native_rumble=" << std::fixed << std::setprecision(6)
               << match.output.values.rumble.low_frequency << ','
               << match.output.values.rumble.high_frequency;
    }
    return output.str();
}

std::string format_rumble_cleanup_zero(const rumble_step_report& report, const bool attempted) {
    std::ostringstream output;
    output << "rumble_cleanup_zero purpose=best_effort attempted=" << std::boolalpha << attempted
           << ' ' << format_rumble_step(report);
    return output.str();
}

std::string format_rumble_connection_stop(const output_session_stop_report& report) {
    std::ostringstream output;
    output << "rumble_connection_stop owner_present=" << std::boolalpha
           << report.connection_owner_present
           << " presentation_close_returned=" << report.presentation_close_returned;
    if (!report.connection_owner_present) {
        output << " control_joined=false retirements_acknowledged=false "
                  "executor_shutdown_returned=false";
        return output.str();
    }

    const auto& connection = report.connections;
    output << " phase=" << connection_phase_name(connection.phase)
           << " admission_quiescent=" << connection.admission_quiescent
           << " control_finished=" << connection.control_finished
           << " control_joined=" << connection.control_joined
           << " retirements_acknowledged=" << connection.all_retirements_acknowledged
           << " executor_shutdown_returned=" << connection.executor_shutdown_returned
           << " candidates=" << connection.candidate_obligations
           << " backends=" << connection.backend_obligations
           << " pending_retirements=" << connection.pending_retirements;
    if (connection.failure.has_value()) {
        const auto& failure = connection.failure.value();
        output << " failure_operation=" << operation_name(failure.operation)
               << " failure_key=" << failure.target.device.value
               << " failure_connection=" << failure.target.connection.value;
        if (failure.backend.has_value()) {
            output << " failure_backend=" << failure.backend->incarnation.id.value;
        }
        if (failure.runtime_failure.has_value()) {
            output << " failure_runtime_code="
                   << static_cast<unsigned int>(failure.runtime_failure->code)
                   << " failure_runtime_native_code=0x" << std::hex
                   << failure.runtime_failure->native_code << std::dec;
        }
        if (failure.native_failure.has_value()) {
            output << " failure_native_cause="
                   << static_cast<unsigned int>(failure.native_failure->cause)
                   << " failure_native_code=0x" << std::hex << failure.native_failure->native_code
                   << std::dec;
        }
        if (failure.executor_failure.has_value()) {
            output << " failure_executor_code="
                   << static_cast<unsigned int>(failure.executor_failure.value());
        }
        if (failure.metadata_failure.has_value()) {
            output << " failure_metadata_code="
                   << static_cast<unsigned int>(*failure.metadata_failure);
        }
    }
    return output.str();
}

std::string format_rumble_runtime_stop(const gameinput_stop_report& report) {
    std::ostringstream output;
    output << "rumble_runtime_stop=" << (report.drained ? "drained" : "pending")
           << " callbacks_pending=" << report.pending_callback_owners
           << " metadata_queries=" << report.active_metadata_queries
           << " history_calls=" << report.active_history_calls
           << " history_owners=" << report.pending_history_owners
           << " output_leases=" << report.pending_output_leases
           << " native_cleanup_owners=" << report.pending_native_cleanup_owners
           << " native_api_cleanup=" << std::boolalpha << report.native_api_cleanup_complete;
    return output.str();
}

} // namespace inputnext::gameinput_detail
