#include "dualsense_usb_worker.hpp"

#include "dualsense_usb_candidate_selector.hpp"
#include "dualsense_usb_context.hpp"
#include "dualsense_usb_evidence.hpp"
#include "dualsense_usb_report.hpp"
#include "dualsense_usb_sequence.hpp"
#include "executable_path.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "manual_sequence.hpp"
#include "report.hpp"
#include "rumble_report.hpp"
#include "rumble_session.hpp"
#include "rumble_window.hpp"
#include "worker.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

using diagnostic_clock = std::chrono::steady_clock;
constexpr auto readiness_report_interval = std::chrono::milliseconds{250};
constexpr auto renewal_interval = std::chrono::milliseconds{2'000};
constexpr auto step_evidence_limit = std::chrono::milliseconds{1'500};
constexpr auto cleanup_evidence_limit = std::chrono::milliseconds{1'000};
constexpr auto diagnostic_ttl = std::chrono::milliseconds{5'000};

struct active_step final {
    const dualsense_usb_step* definition{};
    dualsense_usb_expectation expected{};
    output::backend_handle backend{};
    std::optional<output::attempt_id> before_attempt;
    output::mutation_receipt receipt{};
    diagnostic_clock::time_point deadline;
    diagnostic_clock::time_point renew_at;
};

[[nodiscard]] std::string_view model_name(const metadata_device_model model) noexcept {
    switch (model) {
    case metadata_device_model::generic_gamepad:
        return "generic_gamepad";
    case metadata_device_model::dualsense:
        return "dualsense";
    case metadata_device_model::dualsense_edge:
        return "dualsense_edge";
    }
    return "unknown";
}

[[nodiscard]] dualsense_usb_expectation expectation_for(
    const output::channel_values& values) noexcept {
    return {.logical_channels = output::supported_channel_mask,
        .logical_values = values,
        .native_output = {.channels = output::supported_channel_mask, .values = values}};
}

[[nodiscard]] dualsense_usb_expectation cleared_neutral_expectation() noexcept {
    return {.logical_channels = 0U,
        .logical_values = {},
        .native_output = {.channels = output::supported_channel_mask, .values = {}}};
}

void wait_briefly() noexcept {
    static_cast<void>(
        MsgWaitForMultipleObjectsEx(0U, nullptr, 4U, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
}

class dualsense_usb_probe_run final {
public:
    dualsense_usb_probe_run(const probe_options& options, std::ostream& output)
        : options_(options),
          output_(output),
          window_({.title = L"InputNext DualSense USB Diagnostic",
              .instructions =
                  L"Connect a DualSense or DualSense Edge by USB. Click Start while this window is "
                  L"foreground. Check each effect, then choose Works or Does not work to continue. "
                  L"Abort stops the sequence."}),
          sequence_(dualsense_usb_steps().size(), options.first_step - 1U) {}

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

        deadline_ = options_.duration_ms.has_value()
                        ? diagnostic_clock::now() + std::chrono::milliseconds{*options_.duration_ms}
                        : diagnostic_clock::time_point::max();
        next_readiness_report_ = diagnostic_clock::now();
        window_.set_status("Waiting for a USB-connected DualSense and foreground Start click.");
        output_ << "start=ok output_mode=dualsense_usb_foreground_ui runtime_library=app-local\n"
                << "dualsense_usb_ui visible=true foreground_forced=false "
                   "start=waiting_for_user\n";
        return true;
    }

    void run_foreground_loop() {
        while (diagnostic_clock::now() < deadline_ && window_.valid() && !sequence_.complete()) {
            if (!process_iteration()) {
                break;
            }
            wait_briefly();
        }
        if (!sequence_started_ && incomplete_reason_.empty()) {
            incomplete_reason_ = window_.abort_requested() ? "user_abort" : "start_not_clicked";
        } else if (sequence_started_ && !sequence_.complete() && incomplete_reason_.empty()) {
            incomplete_reason_ = window_.abort_requested() ? "user_abort" : "duration_elapsed";
        }
    }

    [[nodiscard]] bool process_iteration() {
        static_cast<void>(window_.pump_messages());
        if (window_.abort_requested()) {
            if (sequence_started_ && !sequence_.complete()) {
                incomplete_reason_ = "user_abort";
            }
            return false;
        }

        const auto now = diagnostic_clock::now();
        const bool updated = sequence_started_ && selected_.has_value()
                                 ? session_->update_targeted_output(now, selected_->target)
                                 : session_->update(now, false);
        if (!updated) {
            if (sequence_started_) {
                incomplete_reason_ = "composition_synchronize_failed";
            }
            return false;
        }

        if (sequence_started_) {
            return advance_sequence();
        }

        const auto candidate = candidate_selector_.inspect(*session_, now);
        report_readiness(now, candidate);
        return !window_.take_start_request() || handle_start_request(candidate);
    }

    void report_readiness(const maintenance_time now,
        const std::optional<dualsense_usb_candidate>& candidate) {
        if (now < next_readiness_report_ || sequence_started_) {
            return;
        }
        output_ << "dualsense_usb_readiness=" << (candidate.has_value() ? "ready" : "waiting")
                << " window_foreground=" << (window_.is_foreground() ? "true" : "false")
                << " focus_foreground="
                << (session_->focus_is_current_foreground() ? "true" : "false");
        if (candidate.has_value()) {
            output_ << " model=" << model_name(candidate->model)
                    << " target=" << format_rumble_target(candidate->target)
                    << " catalogue_generation=" << candidate->catalogue_generation
                    << " usb_qualified=true";
        }
        output_ << '\n';
        output_.flush();
        next_readiness_report_ = now + readiness_report_interval;
    }

    [[nodiscard]] bool handle_start_request(
        const std::optional<dualsense_usb_candidate>& candidate) {
        start_clicked_ = true;
        if (!candidate.has_value() || !window_.is_foreground() ||
            !session_->focus_is_current_foreground()) {
            window_.set_status("Start ignored. Wait for a USB-qualified controller and keep this "
                               "window in the foreground.");
            output_ << "dualsense_usb_start=ignored reason=foreground_or_usb_device_not_ready\n";
            output_.flush();
            return true;
        }

        const auto description = session_->describe(candidate->target);
        if (!description.has_value() || description->target != candidate->target ||
            description->catalogue_generation != candidate->catalogue_generation ||
            !description->model.has_value() || *description->model != candidate->model) {
            output_ << "dualsense_usb_start=ignored reason=device_metadata_changed\n";
            output_.flush();
            return true;
        }

        const auto opened = diagnostic_context_.open(*session_, candidate->target);
        if (!opened.has_value()) {
            output_ << "dualsense_usb_start=ignored reason=context_open_failed code="
                    << static_cast<unsigned int>(opened.error()) << '\n';
            output_.flush();
            return true;
        }

        selected_ = candidate;
        sequence_started_ = true;
        window_.set_running(true);
        window_.set_status("Start accepted. Opening the selected USB output backend.");
        output_ << "dualsense_usb_start=accepted model=" << model_name(candidate->model)
                << " target=" << format_rumble_target(candidate->target)
                << " catalogue_generation=" << candidate->catalogue_generation
                << " context=" << diagnostic_context_.context()->identity.id.value
                << " steps=" << dualsense_usb_steps().size()
                << " first_step=" << options_.first_step << '\n';
        output_.flush();
        if (!session_->update_targeted_output(diagnostic_clock::now(), candidate->target)) {
            incomplete_reason_ = "output_connection_synchronize_failed";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool sequence_environment_is_current() {
        if (!selected_.has_value() || !diagnostic_context_.is_open()) {
            incomplete_reason_ = "diagnostic_context_missing";
            return false;
        }
        if (!window_.is_foreground() || !session_->focus_is_current_foreground()) {
            incomplete_reason_ = "foreground_lost";
            return false;
        }
        const auto inventory = session_->inventory();
        const bool target_present =
            std::ranges::any_of(std::span{inventory.entries.data(), inventory.size},
                [this](const auto& connection) { return connection.target == selected_->target; });
        if (!target_present) {
            incomplete_reason_ = "device_disconnected";
            return false;
        }
        const auto description = session_->describe(selected_->target);
        if (!description.has_value() || description->target != selected_->target ||
            description->catalogue_generation != selected_->catalogue_generation ||
            !description->model.has_value() || *description->model != selected_->model) {
            incomplete_reason_ = "device_identity_or_metadata_changed";
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

    [[nodiscard]] bool advance_sequence() {
        if (!sequence_environment_is_current() || !selected_.has_value()) {
            return false;
        }
        const auto selected = *selected_;
        const auto now = diagnostic_clock::now();
        if (active_.has_value()) {
            if (const auto confirmation = window_.take_step_result(); confirmation.has_value()) {
                const bool keep_running = accept_user_result(*confirmation);
                if (!keep_running || !active_.has_value()) {
                    return keep_running;
                }
            }
            if (now >= active_->renew_at) {
                sequence_.mark_pending();
                window_.disable_confirmation();
                const auto progress = session_->backend_progress(selected.target);
                if (!progress.has_value()) {
                    incomplete_reason_ = "backend_unavailable_during_renewal";
                    return false;
                }
                begin_step(*active_->definition, *progress, now);
                if (!incomplete_reason_.empty()) {
                    return false;
                }
            } else if (sequence_.awaiting_confirmation(now)) {
                return true;
            }
            return observe_active_step(now);
        }

        const auto steps = dualsense_usb_steps();
        if (sequence_.complete()) {
            return true;
        }
        const auto progress = session_->backend_progress(selected.target);
        if (!progress.has_value() || progress->backend.target != selected.target ||
            progress->phase != output::backend_phase::current ||
            progress->represented_channels != output::supported_channel_mask ||
            progress->unsupported_channels != 0U) {
            window_.set_status("Waiting for the exact DualSense USB output backend.");
            return true;
        }

        begin_step(steps[sequence_.index()], *progress, now);
        return active_.has_value() && observe_active_step(now);
    }

    void begin_step(const dualsense_usb_step& step,
        const output::backend_progress& progress,
        const diagnostic_clock::time_point now) {
        if (!diagnostic_context_.is_open()) {
            incomplete_reason_ = "diagnostic_context_missing";
            return;
        }
        auto receipt = diagnostic_context_.submit(*session_, step.values, diagnostic_ttl);
        if (!receipt.has_value()) {
            incomplete_reason_ = "logical_submit_failed";
            output_ << "dualsense_usb_step=" << step.name << " evidence=unconfirmed "
                    << "submit_error=" << static_cast<unsigned int>(receipt.error()) << '\n';
            output_.flush();
            return;
        }

        active_ = active_step{.definition = &step,
            .expected = expectation_for(step.values),
            .backend = progress.backend,
            .before_attempt = progress.last_attempt,
            .receipt = *receipt,
            .deadline = now + step_evidence_limit,
            .renew_at = now + renewal_interval};
        sequence_.mark_pending();
        window_.disable_confirmation();
        window_.set_status("Step " + std::to_string(sequence_.index() + 1U) + " of " +
                           std::to_string(dualsense_usb_steps().size()) + ". " +
                           std::string{step.instruction} + " Preparing effect...");
    }

    [[nodiscard]] bool accept_user_result(const manual_step_confirmation& confirmation) {
        if (!active_.has_value()) {
            return true;
        }
        if (!sequence_.record_result(confirmation, diagnostic_clock::now())) {
            return true;
        }
        output_ << "dualsense_usb_user_result step=" << active_->definition->name << " result="
                << (confirmation.result == manual_step_result::passed ? "passed" : "failed")
                << '\n';
        output_.flush();
        window_.disable_confirmation();
        active_.reset();
        return !sequence_.complete();
    }

    [[nodiscard]] bool observe_active_step(const diagnostic_clock::time_point now) {
        if (!active_.has_value() || !selected_.has_value() || !diagnostic_context_.is_open()) {
            return false;
        }
        const auto instruction = active_->definition->instruction;
        const auto ready_until = active_->renew_at;
        const auto progress = session_->backend_progress(selected_->target);
        const auto logical = session_->logical_snapshot(selected_->target);
        if (progress.has_value()) {
            dualsense_usb_match matched{};
            const auto evidence = assess_dualsense_usb_evidence(active_->backend,
                active_->before_attempt,
                active_->expected,
                *progress,
                logical.has_value() ? std::optional{*logical} : std::nullopt,
                previous_match_,
                matched);
            if (evidence == dualsense_usb_evidence::rejected) {
                report_step(evidence, std::nullopt);
                output_ << format_dualsense_usb_progress(progress) << '\n';
                incomplete_reason_ = "native_output_rejected_or_backend_changed";
                return false;
            }
            if (evidence == dualsense_usb_evidence::accepted_newer_attempt ||
                evidence == dualsense_usb_evidence::historical_reuse) {
                previous_match_ = matched;
                report_step(evidence, matched);
                output_.flush();
                const auto readiness = sequence_.mark_ready(ready_until, now);
                window_.set_status("Step " + std::to_string(sequence_.index() + 1U) + " of " +
                                   std::to_string(dualsense_usb_steps().size()) + ". " +
                                   std::string{instruction} +
                                   " Choose Works or Does not work when ready.");
                if (readiness.has_value()) {
                    window_.enable_confirmation(*readiness);
                } else {
                    window_.disable_confirmation();
                }
                return true;
            }
        }
        if (now >= active_->deadline) {
            report_step(dualsense_usb_evidence::pending, std::nullopt);
            output_ << format_dualsense_usb_progress(progress) << '\n';
            incomplete_reason_ = "native_evidence_timeout";
            return false;
        }
        return true;
    }

    void report_step(const dualsense_usb_evidence evidence,
        const std::optional<dualsense_usb_match>& match) {
        if (!active_.has_value() || !selected_.has_value() || !diagnostic_context_.is_open()) {
            return;
        }
        output_ << format_dualsense_usb_step({.step = active_->definition->name,
                       .target = selected_->target,
                       .step_number = sequence_.index() + 1U,
                       .step_count = dualsense_usb_steps().size(),
                       .context = diagnostic_context_.context()->identity,
                       .accepted_sequence = active_->receipt.accepted_sequence,
                       .logical_version = active_->receipt.logical_version,
                       .foreground =
                           window_.is_foreground() && session_->focus_is_current_foreground(),
                       .evidence = evidence,
                       .match = match})
                << '\n';
    }

    [[nodiscard]] bool wait_for_evidence(const dualsense_usb_expectation& expected,
        const output::backend_handle& backend,
        const std::optional<output::attempt_id> before_attempt,
        const std::optional<dualsense_usb_match>& previous,
        const diagnostic_clock::time_point deadline,
        dualsense_usb_evidence& observed_evidence,
        dualsense_usb_match& matched) {
        do {
            static_cast<void>(
                session_->update_targeted_output(maintenance_time::clock::now(), backend.target));
            const auto progress = session_->backend_progress(backend.target);
            const auto logical = session_->logical_snapshot(backend.target);
            if (progress.has_value()) {
                const auto evidence = assess_dualsense_usb_evidence(backend,
                    before_attempt,
                    expected,
                    *progress,
                    logical.has_value() ? std::optional{*logical} : std::nullopt,
                    previous,
                    matched);
                if (evidence == dualsense_usb_evidence::accepted_newer_attempt ||
                    evidence == dualsense_usb_evidence::historical_reuse) {
                    observed_evidence = evidence;
                    return true;
                }
                if (evidence == dualsense_usb_evidence::rejected) {
                    observed_evidence = evidence;
                    return false;
                }
            }
            if (diagnostic_clock::now() >= deadline) {
                return false;
            }
            wait_briefly();
        } while (true);
    }

    [[nodiscard]] bool submit_and_observe_neutral() {
        if (!diagnostic_context_.is_open() || !selected_.has_value()) {
            return false;
        }
        const auto before = session_->backend_progress(selected_->target);
        if (!before.has_value() || before->phase != output::backend_phase::current ||
            before->represented_channels != output::supported_channel_mask ||
            before->unsupported_channels != 0U) {
            output_ << "dualsense_usb_cleanup=unconfirmed reason=backend_unavailable\n";
            return false;
        }
        const auto receipt =
            diagnostic_context_.submit(*session_, dualsense_usb_neutral_values(), diagnostic_ttl);
        if (!receipt.has_value()) {
            output_ << "dualsense_usb_cleanup=unconfirmed reason=neutral_submit_failed code="
                    << static_cast<unsigned int>(receipt.error()) << '\n';
            return false;
        }
        dualsense_usb_evidence evidence = dualsense_usb_evidence::pending;
        dualsense_usb_match matched{};
        const bool confirmed = wait_for_evidence(expectation_for(dualsense_usb_neutral_values()),
            before->backend,
            before->last_attempt,
            previous_match_,
            diagnostic_clock::now() + cleanup_evidence_limit,
            evidence,
            matched);
        output_ << "dualsense_usb_cleanup_neutral=" << (confirmed ? "confirmed" : "unconfirmed");
        if (confirmed) {
            previous_match_ = matched;
            output_ << " evidence=" << dualsense_usb_evidence_name(evidence);
            output_ << " attempt=" << matched.attempt.value
                    << " native_status=" << dualsense_usb_status_name(matched.result.status)
                    << " native_code=" << matched.result.native_code;
        }
        output_ << '\n';
        return confirmed;
    }

    [[nodiscard]] bool clear_close_and_acknowledge() {
        if (!diagnostic_context_.is_open()) {
            return true;
        }
        bool cleared = false;
        if (diagnostic_context_.latest_receipt() != nullptr && selected_.has_value()) {
            const auto before = session_->backend_progress(selected_->target);
            const auto receipt = diagnostic_context_.clear(*session_);
            if (receipt.has_value() && before.has_value()) {
                dualsense_usb_evidence evidence = dualsense_usb_evidence::pending;
                dualsense_usb_match matched{};
                cleared = wait_for_evidence(cleared_neutral_expectation(),
                    before->backend,
                    before->last_attempt,
                    previous_match_,
                    diagnostic_clock::now() + cleanup_evidence_limit,
                    evidence,
                    matched);
                if (cleared) {
                    previous_match_ = matched;
                    output_ << "dualsense_usb_clear=confirmed evidence="
                            << dualsense_usb_evidence_name(evidence)
                            << " attempt=" << matched.attempt.value << '\n';
                }
            }
        }

        const auto close = diagnostic_context_.close_and_acknowledge();
        output_ << "dualsense_usb_context_cleanup="
                << (cleared && close.acknowledged ? "confirmed" : "unconfirmed")
                << " clear=" << (cleared ? "confirmed" : "unconfirmed")
                << " close=" << (close.close_submitted ? "confirmed" : "unconfirmed")
                << " acknowledge=" << (close.acknowledged ? "confirmed" : "unconfirmed") << '\n';
        return cleared && close.acknowledged;
    }

    [[nodiscard]] bool best_effort_cleanup() {
        if (!diagnostic_context_.is_open()) {
            return true;
        }
        const bool neutral_confirmed = sequence_.complete() || submit_and_observe_neutral();
        const bool context_confirmed = clear_close_and_acknowledge();
        return neutral_confirmed && context_confirmed;
    }

    [[nodiscard]] int finish() {
        bool cleanup_confirmed = true;
        if (diagnostic_context_.is_open()) {
            cleanup_confirmed = best_effort_cleanup();
        }
        if (!sequence_started_ && incomplete_reason_.empty()) {
            incomplete_reason_ = start_clicked_ ? "start_not_accepted" : "user_start_not_clicked";
        }
        if (sequence_started_ && !sequence_.complete() && incomplete_reason_.empty()) {
            incomplete_reason_ = "sequence_incomplete";
        }

        window_.set_running(false);
        window_.set_status(
            sequence_.complete() && cleanup_confirmed
                ? "Sequence complete. Native acceptance is not a physical measurement."
                : "Sequence incomplete. Cleanup evidence is reported in the diagnostic output.");
        const auto stop = session_->shutdown();
        report_shutdown(stop);
        const bool shutdown_confirmed = runtime_and_connections_drained(stop);
        if (!shutdown_confirmed && incomplete_reason_.empty()) {
            incomplete_reason_ = "shutdown_drain_unverified";
        }

        const bool complete = sequence_.complete() && cleanup_confirmed && shutdown_confirmed;
        output_ << "dualsense_usb_sequence=" << (complete ? "complete" : "incomplete")
                << " steps=" << dualsense_usb_steps().size()
                << " first_step=" << options_.first_step << " user_passed=" << sequence_.passed()
                << " user_failed=" << sequence_.failed()
                << " cleanup=" << (cleanup_confirmed ? "confirmed" : "unconfirmed")
                << " shutdown_drained=" << (shutdown_confirmed ? "true" : "false")
                << " physical_effect=not_measured";
        if (!complete) {
            output_ << " reason=" << incomplete_reason_;
        }
        output_ << '\n';
        output_.flush();
        window_.close();
        return complete && sequence_.failed() == 0U ? 0 : 4;
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
            bool cleanup_confirmed = true;
            if (diagnostic_context_.is_open()) {
                try {
                    cleanup_confirmed = best_effort_cleanup();
                } catch (...) {
                    cleanup_confirmed = false;
                    output_ << "dualsense_usb_cleanup=unconfirmed reason=exception\n";
                }
            }
            if (diagnostic_context_.is_open()) {
                try {
                    static_cast<void>(diagnostic_context_.close_and_acknowledge());
                } catch (...) {
                    cleanup_confirmed = false;
                }
            }
            const auto stop = session_->shutdown();
            report_shutdown(stop);
            output_ << "dualsense_usb_failure_cleanup="
                    << (cleanup_confirmed ? "confirmed" : "unconfirmed") << '\n';
        }
        output_.flush();
        window_.close();
        return 3;
    }

    const probe_options& options_;
    std::ostream& output_;
    rumble_window window_;
    std::unique_ptr<rumble_session> session_;
    dualsense_usb_candidate_selector candidate_selector_;
    std::optional<dualsense_usb_candidate> selected_;
    dualsense_usb_context diagnostic_context_;
    std::optional<dualsense_usb_match> previous_match_;
    std::optional<active_step> active_;
    diagnostic_clock::time_point deadline_;
    diagnostic_clock::time_point next_readiness_report_;
    manual_sequence sequence_;
    bool start_clicked_{};
    bool sequence_started_{};
    std::string incomplete_reason_;
};

} // namespace

int run_dualsense_usb_probe_worker(const probe_options& options, std::ostream& output) {
    return dualsense_usb_probe_run{options, output}.run();
}

} // namespace inputnext::gameinput_detail
