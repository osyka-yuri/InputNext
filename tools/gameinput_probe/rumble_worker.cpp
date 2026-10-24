#include "rumble_worker.hpp"

#include "dualsense_usb_candidate_selector.hpp"
#include "executable_path.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "manual_sequence.hpp"
#include "report.hpp"
#include "rumble_evidence.hpp"
#include "rumble_report.hpp"
#include "rumble_sequence.hpp"
#include "rumble_session.hpp"
#include "rumble_window.hpp"
#include "worker.hpp"
#include "xinput_report.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput_types.hpp>

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

using rumble_clock = std::chrono::steady_clock;
constexpr auto report_interval = std::chrono::milliseconds{250};
constexpr auto step_evidence_limit = std::chrono::milliseconds{1250};
constexpr auto cleanup_evidence_limit = std::chrono::milliseconds{750};

[[nodiscard]] rumble_readiness inspect_readiness(rumble_session& session,
    rumble_window const& window,
    dualsense_usb_candidate_selector& selector,
    const maintenance_time now) {
    const auto positions = session.assignments();
    std::optional<rumble_readiness> fallback;
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        const auto& position = positions[index];
        if (!position.has_value()) {
            continue;
        }
        const auto& assignment = *position;
        rumble_readiness candidate{};
        candidate.assignment = assignment;
        candidate.foreground = session.focus_is_current_foreground();
        candidate.window_foreground = window.is_foreground();
        const auto status = session.output_status(static_cast<std::uint32_t>(index));
        candidate.configured = status.has_value() && status->configured;
        const auto description = session.describe(assignment.target);
        if (description.has_value() && description->target == assignment.target &&
            description->capabilities.has_value()) {
            candidate.low_motor = description->capabilities->low_frequency;
            candidate.high_motor = description->capabilities->high_frequency;
            if (needs_usb_qualification(description->model)) {
                candidate.configured =
                    candidate.configured && selector.is_eligible(session,
                                                assignment.target,
                                                description->catalogue_generation,
                                                now);
            }
        }
        if (!fallback.has_value()) {
            fallback = candidate;
        }
        if (candidate.ready()) {
            return candidate;
        }
    }
    return fallback.value_or(rumble_readiness{});
}

[[nodiscard]] bool logical_state_confirmed(rumble_session const& session,
    const xinput::assignment& selected,
    const rumble_step& step,
    const std::optional<xinput::rumble_output_status>& before,
    const std::optional<xinput::rumble_output_status>& after,
    const std::optional<std::uint32_t> result) {
    const auto logical = session.logical_snapshot(selected.target);
    if (!logical.has_value()) {
        return false;
    }
    const auto& snapshot = *logical;
    const auto* const channel = snapshot.rumble.has_value() ? &*snapshot.rumble : nullptr;
    const bool channel_present = channel != nullptr;
    const auto channel_version =
        channel_present ? std::optional{channel->provenance.accepted_commit_order} : std::nullopt;
    const auto value = channel_present ? channel->value : output::rumble_value{};
    return assess_logical_evidence(step,
        before,
        after,
        result,
        true,
        channel_present,
        value,
        channel_version);
}

struct active_step final {
    const rumble_step* definition{};
    rumble_clock::time_point deadline;
    output::backend_handle expected_backend{};
    std::optional<output::attempt_id> before_attempt;
    std::optional<xinput::rumble_output_status> before_status;
    std::optional<std::uint32_t> logical_result;
    bool logical_call_made{};
    bool report_emitted{};
};

[[nodiscard]] std::optional<active_step> begin_step(rumble_session& session,
    const xinput::assignment& selected,
    const rumble_step& step,
    const std::optional<output::backend_progress>& progress,
    const rumble_clock::time_point now,
    std::uint64_t& logical_call_count) {
    active_step current{};
    current.definition = &step;
    current.deadline = now + step_evidence_limit;
    if (progress.has_value()) {
        current.expected_backend = progress->backend;
    }
    current.before_status = session.output_status(selected.user_index);
    if (progress.has_value()) {
        current.before_attempt = progress->last_attempt;
    }

    switch (step.action) {
    case rumble_step_action::set_state:
    case rumble_step_action::set_disabled_cache: {
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = step.left_motor,
            .wRightMotorSpeed = step.right_motor};
        current.logical_result =
            session.presentation().set_state(xinput::flavor::v1_4, selected.user_index, &vibration);
        current.logical_call_made = true;
        break;
    }
    case rumble_step_action::disable:
        session.presentation().enable(FALSE);
        current.logical_call_made = true;
        break;
    case rumble_step_action::enable:
        session.presentation().enable(TRUE);
        current.logical_call_made = true;
        break;
    }
    if (current.logical_call_made) {
        ++logical_call_count;
    }

    if (step.action != rumble_step_action::set_disabled_cache) {
        session.maintain_output();
        session.request_drive();
    }
    return current;
}

[[nodiscard]] bool same_selected_assignment(const rumble_session& session,
    const xinput::assignment& selected) {
    const auto positions = session.assignments();
    if (selected.user_index >= positions.size()) {
        return false;
    }
    const auto current = positions[selected.user_index];
    return current.has_value() && *current == selected;
}

[[nodiscard]] bool capabilities_still_match(const rumble_session& session,
    const xinput::assignment& selected,
    const rumble_motor_selection selected_motors) {
    const auto description = session.describe(selected.target);
    if (!description.has_value() || description->target != selected.target ||
        !description->capabilities.has_value()) {
        return false;
    }
    const auto& capabilities = *description->capabilities;
    return (!selected_motors.low_motor || capabilities.low_frequency) &&
           (!selected_motors.high_motor || capabilities.high_frequency);
}

[[nodiscard]] rumble_step_report make_step_report(const rumble_session& session,
    const rumble_window& window,
    const xinput::assignment& selected,
    const active_step& active,
    const rumble_motor_selection selected_motors,
    const rumble_native_evidence evidence,
    const std::optional<rumble_native_match>& native_match,
    const bool logical_confirmed,
    const bool capability_confirmed) {
    const auto status = session.output_status(selected.user_index);
    const auto logical = session.logical_snapshot(selected.target);
    return {.step = active.definition->name,
        .target = selected.target,
        .assignment_id = selected.id,
        .request_revision = status.has_value() ? status->request_revision : 0U,
        .logical_version = status.has_value() ? status->logical_version : 0U,
        .logical_channel_version =
            logical.has_value() && logical->rumble.has_value()
                ? std::optional{logical->rumble->provenance.accepted_commit_order}
                : std::nullopt,
        .left_motor = active.definition->left_motor,
        .right_motor = active.definition->right_motor,
        .logical_channel_present = logical.has_value() && logical->rumble.has_value(),
        .logical_result = active.logical_result,
        .logical_call_made = active.logical_call_made,
        .logical_state_confirmed = logical_confirmed,
        .assignment_live = same_selected_assignment(session, selected),
        .foreground = window.is_foreground() && session.focus_is_current_foreground(),
        .low_motor_available = selected_motors.low_motor,
        .high_motor_available = selected_motors.high_motor,
        .capabilities_confirmed = capability_confirmed,
        .native_evidence = evidence,
        .native_match = native_match};
}

[[nodiscard]] std::optional<output::backend_progress>
find_ready_backend(rumble_session const& session, const connection_target target) {
    auto progress = session.backend_progress(target);
    if (!progress.has_value() || progress->backend.target != target ||
        progress->phase != output::backend_phase::current || progress->unsupported_channels != 0U ||
        (progress->represented_channels != output::rumble_mask &&
            progress->represented_channels != output::supported_channel_mask)) {
        return std::nullopt;
    }
    return progress;
}

void wait_briefly() noexcept {
    (void)MsgWaitForMultipleObjectsEx(0U, nullptr, 4U, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

struct final_zero_result final {
    bool attempted{};
    bool logical_confirmed{};
    rumble_native_evidence evidence{rumble_native_evidence::pending};
    std::optional<rumble_native_match> match;
};

struct cleanup_zero_state final {
    DWORD setter_result{};
    std::optional<xinput::rumble_output_status> after_status;
    std::optional<output::logical_snapshot> projection;
    bool logical_confirmed{};
};

[[nodiscard]] cleanup_zero_state request_cleanup_zero(rumble_session& session,
    const xinput::assignment& selected,
    const bool sequence_disabled,
    const std::optional<xinput::rumble_output_status>& before_status) {
    auto& presentation = session.presentation();
    const auto vibration = XINPUT_VIBRATION{};
    cleanup_zero_state state{};
    state.setter_result =
        presentation.set_state(xinput::flavor::v1_4, selected.user_index, &vibration);
    state.after_status = session.output_status(selected.user_index);
    if (sequence_disabled && before_status.has_value() && state.after_status.has_value() &&
        state.after_status->request_revision > before_status->request_revision &&
        state.after_status->desired_state_cached) {
        presentation.enable(TRUE);
        state.after_status = session.output_status(selected.user_index);
    }
    session.maintain_output();
    session.request_drive();

    auto projection = session.logical_snapshot(selected.target);
    if (projection.has_value()) {
        state.projection = *projection;
    }
    state.logical_confirmed =
        state.setter_result == ERROR_SUCCESS && state.after_status.has_value() &&
        state.after_status->desired_state_cached && before_status.has_value() &&
        state.after_status->request_revision > before_status->request_revision &&
        state.after_status->logical_version != 0U && state.projection.has_value() &&
        state.projection->rumble.has_value() &&
        state.projection->rumble->value == output::rumble_value{} &&
        state.projection->rumble->provenance.accepted_commit_order ==
            state.after_status->logical_version;
    return state;
}

[[nodiscard]] final_zero_result observe_cleanup_zero(rumble_session& session,
    const xinput::assignment& selected,
    const rumble_step& zero_step,
    const std::optional<output::backend_progress>& before_progress,
    const std::optional<rumble_native_match>& previous_match) {
    final_zero_result final{.attempted = true, .match = std::nullopt};
    if (!before_progress.has_value()) {
        return final;
    }

    const auto evidence_deadline = rumble_clock::now() + cleanup_evidence_limit;
    do {
        static_cast<void>(session.update(rumble_clock::now(), true));
        const auto progress = session.backend_progress(selected.target);
        const auto logical = session.logical_snapshot(selected.target);
        if (progress.has_value() && logical.has_value()) {
            rumble_native_match match{};
            const rumble_native_observation observation{.progress = *progress,
                .logical_channel_present = logical->rumble.has_value(),
                .logical_value =
                    logical->rumble.has_value() ? logical->rumble->value : output::rumble_value{}};
            final.evidence = assess_native_evidence(zero_step,
                before_progress->backend,
                before_progress->last_attempt,
                observation,
                previous_match,
                match);
            if (final.evidence == rumble_native_evidence::accepted_newer_attempt ||
                final.evidence == rumble_native_evidence::historical_reuse) {
                final.match = match;
                break;
            }
        }
        if (rumble_clock::now() >= evidence_deadline ||
            !same_selected_assignment(session, selected)) {
            break;
        }
        wait_briefly();
    } while (true);
    return final;
}

void report_cleanup_zero(const rumble_session& session,
    const rumble_window& window,
    const xinput::assignment& selected,
    const rumble_motor_selection selected_motors,
    const cleanup_zero_state& state,
    const final_zero_result& final,
    std::ostream& output) {
    const auto& logical = state.projection;
    output << format_rumble_cleanup_zero(
                  {.step = "cleanup_zero",
                      .target = selected.target,
                      .assignment_id = selected.id,
                      .request_revision = state.after_status.has_value()
                                              ? state.after_status->request_revision
                                              : 0U,
                      .logical_version =
                          state.after_status.has_value() ? state.after_status->logical_version : 0U,
                      .logical_channel_version =
                          logical.has_value() && logical->rumble.has_value()
                              ? std::optional{logical->rumble->provenance.accepted_commit_order}
                              : std::nullopt,
                      .left_motor = 0U,
                      .right_motor = 0U,
                      .logical_channel_present = logical.has_value() && logical->rumble.has_value(),
                      .logical_result = state.setter_result,
                      .logical_call_made = true,
                      .logical_state_confirmed = state.logical_confirmed,
                      .assignment_live = same_selected_assignment(session, selected),
                      .foreground = window.is_foreground() && session.focus_is_current_foreground(),
                      .low_motor_available = selected_motors.low_motor,
                      .high_motor_available = selected_motors.high_motor,
                      .capabilities_confirmed =
                          capabilities_still_match(session, selected, selected_motors),
                      .native_evidence = final.evidence,
                      .native_match = final.match},
                  final.attempted)
           << '\n';
}

[[nodiscard]] final_zero_result best_effort_zero(rumble_session& session,
    const rumble_window& window,
    const std::optional<xinput::assignment>& selected,
    const rumble_motor_selection selected_motors,
    const rumble_step& zero_step,
    const bool sequence_disabled,
    const std::optional<rumble_native_match>& previous_match,
    std::ostream& output) {
    if (!selected.has_value() || !same_selected_assignment(session, selected.value())) {
        output
            << "rumble_cleanup_zero attempted=false assignment_live=false evidence=unavailable\n";
        return {};
    }

    const auto before_status = session.output_status(selected->user_index);
    const auto before_progress = session.backend_progress(selected->target);
    const auto state = request_cleanup_zero(session, *selected, sequence_disabled, before_status);
    const auto final =
        observe_cleanup_zero(session, *selected, zero_step, before_progress, previous_match);
    report_cleanup_zero(session, window, *selected, selected_motors, state, final, output);
    return final;
}

class rumble_worker_run final {
public:
    rumble_worker_run(const probe_options& options, std::ostream& output)
        : options_(options),
          output_(output) {}

    [[nodiscard]] int run() {
        try {
            if (!start_session()) {
                return 3;
            }
            run_foreground_loop();
            return finish();
        } catch (const std::exception& error) {
            return report_failure(error.what());
        } catch (...) {
            return report_failure("unknown_exception");
        }
    }

private:
    [[nodiscard]] bool start_session() {
        const auto library = current_executable_path().parent_path() / L"GameInputRedist.dll";
        session_ = std::make_unique<rumble_session>(library);
        const auto started = session_->start();
        if (!started.has_value()) {
            output_ << format_start_failure(started.error()) << '\n';
            report_shutdown(session_->shutdown());
            output_.flush();
            return false;
        }

        session_started_ = true;
        deadline_ = options_.duration_ms.has_value()
                        ? rumble_clock::now() + std::chrono::milliseconds{*options_.duration_ms}
                        : rumble_clock::time_point::max();
        next_report_ = rumble_clock::now();
        output_ << "start=ok output_mode=foreground_rumble_ui runtime_library=app-local\n"
                << "rumble_ui visible=true foreground_forced=false start=waiting_for_user\n";
        window_.set_status(
            "Waiting for a connected controller with rumble support. Bring this window forward "
            "and click Start.");
        return true;
    }

    [[nodiscard]] bool sequence_complete() const noexcept {
        return manual_sequence_.has_value() && manual_sequence_->complete();
    }

    [[nodiscard]] std::size_t user_passed() const noexcept {
        return manual_sequence_.has_value() ? manual_sequence_->passed() : 0U;
    }

    [[nodiscard]] std::size_t user_failed() const noexcept {
        return manual_sequence_.has_value() ? manual_sequence_->failed() : 0U;
    }

    void run_foreground_loop() {
        while (rumble_clock::now() < deadline_ && window_.valid()) {
            if (!process_iteration()) {
                return;
            }
            wait_briefly();
        }
        if (sequence_started_ && !sequence_complete() && incomplete_reason_.empty()) {
            incomplete_reason_ = window_.abort_requested() ? "user_abort" : "duration_elapsed";
        }
    }

    [[nodiscard]] bool process_iteration() {
        static_cast<void>(window_.pump_messages());
        if (window_.abort_requested()) {
            if (sequence_started_ && !sequence_complete()) {
                incomplete_reason_ = "user_abort";
            }
            return false;
        }

        const auto now = rumble_clock::now();
        if (!session_->update(now, output_started_)) {
            if (sequence_started_) {
                incomplete_reason_ = "composition_synchronize_failed";
            }
            return false;
        }

        if (!sequence_started_) {
            const auto readiness = inspect_readiness(*session_, window_, selector_, now);
            report_readiness(now, readiness);
            if (window_.take_start_request() && !handle_start_request(readiness)) {
                return false;
            }
        }
        return !sequence_started_ || advance_sequence(now);
    }

    void report_readiness(const rumble_clock::time_point now, const rumble_readiness& readiness) {
        if (now < next_report_ || sequence_started_) {
            return;
        }
        const auto focus = session_->focus_snapshot();
        output_ << format_xinput_focus(focus) << '\n'
                << format_rumble_readiness(readiness, focus) << '\n';
        output_.flush();
        next_report_ = now + report_interval;
    }

    [[nodiscard]] bool handle_start_request(const rumble_readiness& readiness) {
        start_clicked_ = true;
        if (!readiness.ready() || !readiness.assignment.has_value()) {
            window_.set_status(
                "Start ignored. Wait for a controller with rumble support, bring this window "
                "forward, and click Start.");
            output_
                << "rumble_start=ignored reason=foreground_assignment_or_capability_not_ready\n";
            output_.flush();
            return true;
        }

        selected_assignment_ = readiness.assignment;
        selected_motors_ = {.low_motor = readiness.low_motor, .high_motor = readiness.high_motor};
        sequence_ = make_rumble_sequence(selected_motors_);
        manual_sequence_.emplace(sequence_.size());
        sequence_started_ = true;
        output_started_ = true;
        window_.set_running(true);
        window_.set_status("Start accepted. Preparing the first effect.");
        const auto& selected = *selected_assignment_;
        output_ << "rumble_start=accepted user=" << selected.user_index
                << " assignment=" << selected.id << ' ' << format_rumble_target(selected.target)
                << " low_cap=" << (selected_motors_.low_motor ? "true" : "false")
                << " high_cap=" << (selected_motors_.high_motor ? "true" : "false")
                << " steps=" << sequence_.size()
                << " focus_revision=" << session_->focus_snapshot().revision << '\n';
        output_.flush();
        if (session_->update(rumble_clock::now(), true)) {
            return true;
        }
        incomplete_reason_ = "output_connection_synchronize_failed";
        return false;
    }

    [[nodiscard]] bool sequence_environment_is_current() {
        if (!selected_assignment_.has_value()) {
            incomplete_reason_ = "assignment_changed_or_disconnected";
            return false;
        }
        const auto& selected = *selected_assignment_;
        if (!same_selected_assignment(*session_, selected)) {
            incomplete_reason_ = "assignment_changed_or_disconnected";
            return false;
        }
        if (!capabilities_still_match(*session_, selected, selected_motors_)) {
            incomplete_reason_ = "rumble_capability_lost_or_unavailable";
            return false;
        }
        if (!window_.is_foreground() || !session_->focus_is_current_foreground()) {
            incomplete_reason_ = "foreground_lost";
            return false;
        }
        const auto control = session_->connection_progress();
        if (control.phase == connection_control_phase::needs_executor_shutdown ||
            control.phase == connection_control_phase::stopped) {
            incomplete_reason_ = "output_connection_owner_failed";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool advance_sequence(const rumble_clock::time_point now) {
        if (!sequence_environment_is_current()) {
            return false;
        }
        if (!selected_assignment_.has_value() || !manual_sequence_.has_value()) {
            return false;
        }
        const auto& sequence = *manual_sequence_;
        const auto& selected = *selected_assignment_;
        if (current_step_.has_value()) {
            if (const auto confirmation = window_.take_step_result(); confirmation.has_value()) {
                const bool keep_running = accept_user_result(*confirmation);
                if (!keep_running || !current_step_.has_value()) {
                    return keep_running;
                }
            }
            if (sequence.awaiting_confirmation()) {
                return true;
            }
        }
        if (sequence.complete()) {
            return false;
        }
        const auto progress = find_ready_backend(*session_, selected.target);
        if (!progress.has_value()) {
            window_.set_status("Preparing the next effect. Keep this window in the foreground.");
            return true;
        }
        if (!current_step_.has_value()) {
            begin_next_step(selected, *progress, now);
        }
        return !current_step_.has_value() || observe_current_step(now, selected);
    }

    void begin_next_step(const xinput::assignment& selected,
        const output::backend_progress& progress,
        const rumble_clock::time_point now) {
        if (!manual_sequence_.has_value() || manual_sequence_->complete()) {
            return;
        }
        const auto& step = sequence_.steps()[manual_sequence_->index()];
        current_step_ = begin_step(*session_, selected, step, progress, now, logical_calls_);
        manual_sequence_->mark_pending();
        window_.disable_confirmation();
        if (step.action == rumble_step_action::disable) {
            sequence_disabled_ = true;
        } else if (step.action == rumble_step_action::enable) {
            sequence_disabled_ = false;
        }
        const auto number = manual_sequence_->index() + 1U;
        window_.set_status("Step " + std::to_string(number) + "/" +
                           std::to_string(sequence_.size()) + ". " + std::string{step.instruction} +
                           " Preparing the effect.");
    }

    [[nodiscard]] rumble_native_evidence observe_native_step(const active_step& step,
        const xinput::assignment& selected,
        rumble_native_match& matched) const {
        if (!step.definition->expects_native_output) {
            return rumble_native_evidence::not_requested;
        }
        const auto logical = session_->logical_snapshot(selected.target);
        const auto progress = session_->backend_progress(selected.target);
        if (!progress.has_value() || !logical.has_value()) {
            return rumble_native_evidence::pending;
        }
        const bool channel_present = logical->rumble.has_value();
        const auto value = channel_present ? logical->rumble->value : output::rumble_value{};
        return assess_native_evidence(*step.definition,
            step.expected_backend,
            step.before_attempt,
            {.progress = *progress,
                .logical_channel_present = channel_present,
                .logical_value = value},
            previous_match_,
            matched);
    }

    [[nodiscard]] bool observe_current_step(const rumble_clock::time_point now,
        const xinput::assignment& selected) {
        if (!current_step_.has_value()) {
            return true;
        }
        const auto& step = *current_step_;
        const auto after_status = session_->output_status(selected.user_index);
        const bool logical_confirmed = logical_state_confirmed(*session_,
            selected,
            *step.definition,
            step.before_status,
            after_status,
            step.logical_result);
        rumble_native_match matched{};
        const auto evidence = observe_native_step(step, selected, matched);
        if (evidence == rumble_native_evidence::rejected) {
            report_current_step(evidence, std::nullopt, logical_confirmed);
            incomplete_reason_ = "native_output_rejected";
            return false;
        }
        if (logical_confirmed && native_state_confirmed(evidence)) {
            std::optional<rumble_native_match> accepted_match;
            if (evidence == rumble_native_evidence::accepted_newer_attempt ||
                evidence == rumble_native_evidence::historical_reuse) {
                accepted_match = matched;
                previous_match_ = matched;
            }
            report_current_step(evidence, accepted_match, logical_confirmed);
            output_.flush();
            if (!manual_sequence_.has_value()) {
                incomplete_reason_ = "manual_sequence_missing";
                return false;
            }
            const auto readiness = manual_sequence_->mark_ready();
            const auto number = manual_sequence_->index() + 1U;
            window_.set_status("Step " + std::to_string(number) + "/" +
                               std::to_string(sequence_.size()) + ". " +
                               std::string{step.definition->instruction} +
                               " Check the effect, then choose Works or Does not work.");
            if (readiness.has_value()) {
                window_.enable_confirmation(*readiness);
            } else {
                window_.disable_confirmation();
            }
            return true;
        }
        if (now >= step.deadline) {
            report_current_step(rumble_native_evidence::pending, std::nullopt, logical_confirmed);
            incomplete_reason_ =
                logical_confirmed ? "native_evidence_timeout" : "logical_status_timeout";
            return false;
        }
        return true;
    }

    [[nodiscard]] static bool native_state_confirmed(const rumble_native_evidence evidence) {
        return evidence == rumble_native_evidence::accepted_newer_attempt ||
               evidence == rumble_native_evidence::historical_reuse ||
               evidence == rumble_native_evidence::not_requested;
    }

    [[nodiscard]] bool accept_user_result(const manual_step_confirmation& confirmation) {
        if (!current_step_.has_value() || !manual_sequence_.has_value() ||
            !manual_sequence_->record_result(confirmation, rumble_clock::now())) {
            return true;
        }
        output_ << "rumble_user_result step=" << current_step_->definition->name << " result="
                << (confirmation.result == manual_step_result::passed ? "passed" : "failed")
                << '\n';
        output_.flush();
        window_.disable_confirmation();
        current_step_.reset();
        if (!manual_sequence_->complete()) {
            window_.set_status("Preparing the next effect. Keep this window in the foreground.");
        }
        return !manual_sequence_->complete();
    }

    void report_current_step(const rumble_native_evidence evidence,
        const std::optional<rumble_native_match>& match,
        const bool logical_confirmed) {
        if (!current_step_.has_value() || current_step_->report_emitted ||
            !selected_assignment_.has_value()) {
            return;
        }
        const auto& selected = *selected_assignment_;
        output_ << format_rumble_step(make_step_report(*session_,
                       window_,
                       selected,
                       *current_step_,
                       selected_motors_,
                       evidence,
                       match,
                       logical_confirmed,
                       capabilities_still_match(*session_, selected, selected_motors_)))
                << '\n';
        current_step_->report_emitted = true;
    }

    void report_pending_step() {
        if (!current_step_.has_value() || current_step_->report_emitted ||
            !selected_assignment_.has_value()) {
            return;
        }
        const auto& selected = *selected_assignment_;
        const auto status = session_->output_status(selected.user_index);
        const bool logical_confirmed = logical_state_confirmed(*session_,
            selected,
            *current_step_->definition,
            current_step_->before_status,
            status,
            current_step_->logical_result);
        report_current_step(rumble_native_evidence::pending, std::nullopt, logical_confirmed);
    }

    void best_effort_cleanup() {
        try {
            report_pending_step();
            static_cast<void>(best_effort_zero(*session_,
                window_,
                selected_assignment_,
                selected_motors_,
                sequence_.steps().back(),
                sequence_disabled_,
                previous_match_,
                output_));
        } catch (const std::exception& error) {
            output_ << "rumble_cleanup_zero attempted=false evidence=unverified message="
                    << error.what() << '\n';
        } catch (...) {
            output_ << "rumble_cleanup_zero attempted=false evidence=unverified\n";
        }
    }

    [[nodiscard]] int finish() {
        if (!sequence_started_) {
            incomplete_reason_ = start_clicked_ ? "start_not_accepted" : "user_start_not_clicked";
            output_ << "rumble_sequence=not_started reason=" << incomplete_reason_
                    << " xinput_logical_calls=" << logical_calls_ << '\n';
        }
        if (sequence_started_ && !sequence_complete()) {
            best_effort_cleanup();
        }

        window_.set_running(false);
        if (!sequence_complete()) {
            window_.set_status("Sequence incomplete. See the diagnostic report.");
        } else if (user_failed() == 0U) {
            window_.set_status("Sequence complete. Every effect was reported as working.");
        } else {
            window_.set_status(
                "Sequence complete. One or more effects were reported as not working.");
        }
        const auto stop = session_->shutdown();
        report_shutdown(stop);
        const bool shutdown_confirmed = runtime_and_connections_drained(stop);
        if (!shutdown_confirmed && incomplete_reason_.empty()) {
            incomplete_reason_ = "cleanup_unverified";
        }
        const bool complete = sequence_complete() && shutdown_confirmed;
        output_ << "rumble_sequence=" << (complete ? "complete" : "incomplete")
                << " steps=" << sequence_.size() << " user_passed=" << user_passed()
                << " user_failed=" << user_failed()
                << " low_cap=" << (selected_motors_.low_motor ? "true" : "false")
                << " high_cap=" << (selected_motors_.high_motor ? "true" : "false")
                << " physical_effect=not_measured";
        if (!complete) {
            output_ << " reason=" << incomplete_reason_;
        }
        output_ << '\n';
        output_.flush();
        window_.close();
        return complete && user_failed() == 0U ? 0 : 4;
    }

    void report_shutdown(const rumble_session_stop_report& stop) {
        output_ << format_rumble_connection_stop(stop) << '\n'
                << format_rumble_runtime_stop(stop.runtime) << '\n';
    }

    [[nodiscard]] static bool runtime_and_connections_drained(
        const rumble_session_stop_report& stop) {
        return stop.presentation_close_returned && stop.runtime_stopped && stop.runtime.drained &&
               (!stop.connection_owner_present ||
                   (stop.connections.control_joined &&
                       stop.connections.executor_shutdown_returned &&
                       stop.connections.all_retirements_acknowledged &&
                       stop.connections.candidate_obligations == 0U &&
                       stop.connections.backend_obligations == 0U));
    }

    [[nodiscard]] int report_failure(const std::string_view message) {
        output_ << "worker=failed message=" << message << '\n';
        if (session_) {
            if (sequence_started_ && !sequence_complete()) {
                best_effort_cleanup();
            }
            report_shutdown(session_->shutdown());
        }
        output_.flush();
        window_.close();
        return 3;
    }

    const probe_options& options_;
    std::ostream& output_;
    rumble_window window_;
    dualsense_usb_candidate_selector selector_;
    std::unique_ptr<rumble_session> session_;
    std::optional<xinput::assignment> selected_assignment_;
    std::optional<rumble_native_match> previous_match_;
    std::optional<active_step> current_step_;
    std::optional<manual_sequence> manual_sequence_;
    rumble_sequence sequence_;
    rumble_motor_selection selected_motors_{};
    rumble_clock::time_point deadline_;
    rumble_clock::time_point next_report_;
    bool session_started_{};
    bool start_clicked_{};
    bool sequence_started_{};
    bool sequence_disabled_{};
    bool output_started_{};
    std::uint64_t logical_calls_{};
    std::string incomplete_reason_;
};

} // namespace

int run_rumble_probe_worker(const probe_options& options, std::ostream& output) {
    return rumble_worker_run{options, output}.run();
}

} // namespace inputnext::gameinput_detail
