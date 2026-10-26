#include "hd_audio_worker.hpp"

#include "dualsense_usb_candidate_selector.hpp"
#include "executable_path.hpp"
#include "hd_audio_candidate.hpp"
#include "hd_audio_sequence.hpp"
#include "hd_audio_step_runner.hpp"
#include "manual_sequence.hpp"
#include "rumble_report.hpp"
#include "rumble_session.hpp"
#include "rumble_window.hpp"
#include "worker.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <ios>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

using diagnostic_clock = std::chrono::steady_clock;
constexpr auto readiness_report_interval = std::chrono::milliseconds{500};
constexpr auto step_evidence_limit = std::chrono::seconds{10};

[[nodiscard]] bool target_is_present(const rumble_session& session,
    const connection_target target) {
    const auto inventory = session.inventory();
    return std::ranges::any_of(std::span{inventory.entries.data(), inventory.size},
        [target](const auto& connection) { return connection.target == target; });
}

[[nodiscard]] std::optional<audio_detail::audio_target_progress> ready_audio(
    const rumble_session& session,
    const connection_target target,
    const output::backend_handle& backend) {
    auto progress = session.hd_audio_progress(target);
    if (!progress.has_value() || progress->target != target || progress->backend != backend ||
        !progress->audio.has_value() || !progress->audio->valid() ||
        progress->audio->target != target ||
        progress->audio->authority != backend.incarnation.owner.authority ||
        progress->phase != audio_detail::audio_target_phase::live || !progress->transport_open ||
        !progress->native_stream_started || !progress->ready) {
        return std::nullopt;
    }
    return progress;
}

void wait_briefly() noexcept {
    static_cast<void>(
        MsgWaitForMultipleObjectsEx(0U, nullptr, 4U, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
}

class hd_audio_probe_run final {
public:
    hd_audio_probe_run(const probe_options& options, std::ostream& output)
        : options_(options),
          output_(output),
          window_({.title = L"InputNext DualSense HD Diagnostic",
              .instructions =
                  L"Connect a DualSense or DualSense Edge by USB. Click Start while this window is "
                  L"foreground. Check each effect, then choose Works or Does not work to continue. "
                  L"Abort stops the sequence."}),
          sequence_(hd_audio_steps().size()) {}

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
    void set_step_status(const std::string_view message) {
        window_.set_status("Step " + std::to_string(sequence_.index() + 1U) + " of " +
                           std::to_string(hd_audio_steps().size()) + ". " + std::string{message});
    }

    [[nodiscard]] bool start_session() {
        const auto library = current_executable_path().parent_path() / L"GameInputRedist.dll";
        session_ = std::make_unique<rumble_session>(library);
        const auto started = session_->start();
        if (!started.has_value()) {
            output_ << "start_failure code=" << static_cast<unsigned int>(started.error().code)
                    << " native_code=" << started.error().native_code << '\n';
            report_shutdown(session_->shutdown());
            output_.flush();
            return false;
        }

        step_runner_ = std::make_unique<hd_audio_step_runner>(*session_, output_);

        deadline_ = options_.duration_ms.has_value()
                        ? diagnostic_clock::now() + std::chrono::milliseconds{*options_.duration_ms}
                        : diagnostic_clock::time_point::max();
        next_readiness_report_ = diagnostic_clock::now();
        window_.set_status("Waiting for a USB-connected DualSense. Click Start sequence to begin.");
        output_ << "start=ok output_mode=dualsense_hd_foreground_ui runtime_library=app-local\n"
                << "dualsense_hd_ui visible=true foreground_forced=false "
                   "start=waiting_for_user audio_started=false usb_setter_before_start=false\n";
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
            incomplete_reason_ = "user_abort";
            return false;
        }
        if (!window_.valid()) {
            incomplete_reason_ = "window_closed";
            return false;
        }
        if (sequence_started_ &&
            (!window_.is_foreground() || !session_->focus_is_current_foreground())) {
            incomplete_reason_ = "foreground_lost";
            window_.disable_confirmation();
            return false;
        }

        const maintenance_time now = maintenance_time::clock::now();
        if (!update_runtime(now)) {
            if (sequence_started_) {
                incomplete_reason_ = "runtime_or_output_synchronize_failed";
                return false;
            }
            return true;
        }
        session_->collect_body_pcm();

        if (!sequence_started_) {
            return process_initial_wait(now);
        }
        if (awaiting_reconnect_) {
            return process_reconnect_wait(now);
        }
        return process_active_session(now);
    }

    [[nodiscard]] bool update_runtime(const maintenance_time now) {
        if (awaiting_reconnect_ || !sequence_started_ || !selected_.has_value()) {
            return session_->update(now, false);
        }
        const auto target = selected_.value().target;
        return session_->update_targeted_output(now, target);
    }

    [[nodiscard]] bool process_initial_wait(const maintenance_time now) {
        const auto candidate = selector_.inspect(*session_, now);
        report_initial_readiness(now, candidate);
        if (!window_.take_start_request()) {
            return true;
        }
        return handle_initial_start(candidate, now);
    }

    [[nodiscard]] bool process_reconnect_wait(const maintenance_time now) {
        const auto candidate = selector_.inspect(*session_, now);
        report_reconnect_readiness(now, candidate);
        if (!window_.take_start_request()) {
            return true;
        }
        return handle_reconnect_start(candidate, now);
    }

    [[nodiscard]] bool process_active_session(const diagnostic_clock::time_point now) {
        if (!selected_.has_value() || step_runner_ == nullptr || !step_runner_->has_context()) {
            incomplete_reason_ = "step_runner_or_context_missing";
            return false;
        }
        const auto target = selected_.value().target;
        if (!target_is_present(*session_, target)) {
            incomplete_reason_ = "selected_usb_target_lost";
            window_.disable_confirmation();
            return false;
        }

        if (step_runner_->has_active_step()) {
            return advance_active_step(now, target);
        }
        if (awaiting_audio_ready_) {
            return wait_for_audio_readiness(now);
        }
        return begin_next_step(now, target);
    }

    [[nodiscard]] bool begin_next_step(const diagnostic_clock::time_point now,
        const connection_target target) {
        const auto backend = step_runner_->current_backend(target);
        if (!backend.has_value()) {
            window_.set_status("Waiting for the selected USB controller.");
            return true;
        }
        const auto audio = ready_audio(*session_, target, backend->backend);
        step_runner_->report_progress(now, target, audio);
        const auto& step = hd_audio_steps()[sequence_.index()];
        if (!audio.has_value() &&
            (step.action == hd_audio_step_action::hd_pulse || !step_runner_->has_receipt())) {
            window_.set_status("Preparing the next effect.");
            return true;
        }
        return begin_step(step, *backend, audio, now);
    }

    void report_initial_readiness(const diagnostic_clock::time_point now,
        const std::optional<dualsense_usb_candidate>& candidate) {
        if (now < next_readiness_report_) {
            return;
        }
        output_ << "dualsense_hd_readiness=" << (candidate.has_value() ? "usb_ready" : "waiting")
                << " window_foreground=" << std::boolalpha << window_.is_foreground()
                << " focus_foreground=" << session_->focus_is_current_foreground();
        if (candidate.has_value()) {
            output_ << " target=" << format_rumble_target(candidate->target)
                    << " catalogue_generation=" << candidate->catalogue_generation
                    << " usb_qualified=true";
        }
        output_ << " audio_started=false\n";
        output_.flush();
        next_readiness_report_ = now + readiness_report_interval;
    }

    void report_reconnect_readiness(const diagnostic_clock::time_point now,
        const std::optional<dualsense_usb_candidate>& candidate) {
        if (now < next_readiness_report_) {
            return;
        }
        output_ << "dualsense_hd_reconnect="
                << (candidate.has_value() ? "usb_candidate" : "waiting") << " original_target=";
        if (original_target_.has_value()) {
            output_ << format_rumble_target(original_target_.value());
        } else {
            output_ << "none";
        }
        if (candidate.has_value()) {
            output_ << " candidate_target=" << format_rumble_target(candidate->target)
                    << " catalogue_generation=" << candidate->catalogue_generation
                    << " usb_qualified=true identity_check=on_start_click";
        }
        output_ << '\n';
        output_.flush();
        next_readiness_report_ = now + readiness_report_interval;
    }

    [[nodiscard]] bool handle_initial_start(const std::optional<dualsense_usb_candidate>& candidate,
        const maintenance_time now) {
        start_clicked_ = true;
        if (!candidate.has_value() || !window_.is_foreground() ||
            !session_->focus_is_current_foreground()) {
            window_.set_status(
                "Start ignored. Wait for USB qualification and keep this window foreground.");
            output_ << "dualsense_hd_start=ignored reason=foreground_or_usb_device_not_ready\n";
            output_.flush();
            return true;
        }
        const auto binding = inspect_binding(*candidate);
        if (!binding.has_value()) {
            window_.set_status("Start ignored. The selected controller is not ready. "
                               "See the diagnostic report.");
            output_ << "dualsense_hd_start=ignored reason=reconnect_identity_unavailable target="
                    << format_rumble_target(candidate->target) << '\n';
            output_.flush();
            return true;
        }
        if (!session_->update_targeted_output(now, candidate->target)) {
            incomplete_reason_ = "output_connection_synchronize_failed";
            return false;
        }
        if (!session_->start_hd_audio(candidate->target)) {
            incomplete_reason_ = "audio_service_start_failed";
            return false;
        }
        const auto opened = step_runner_->open(candidate->target);
        if (!opened.has_value()) {
            copy_step_failure();
            return false;
        }
        selected_ = candidate;
        original_target_ = candidate->target;
        binding_ = *binding;
        sequence_started_ = true;
        awaiting_audio_ready_ = true;
        audio_readiness_deadline_ = diagnostic_clock::now() + step_evidence_limit;
        window_.set_running(true);
        window_.set_status("Start accepted. Preparing the first effect.");
        output_ << "dualsense_hd_start=accepted target=" << format_rumble_target(candidate->target)
                << " catalogue_generation=" << candidate->catalogue_generation
                << " context=" << opened->identity.id.value << " steps=" << hd_audio_steps().size()
                << " haptic_endpoint=present container_id=present "
                   "audio_service_started_after_start=true\n";
        output_.flush();
        return true;
    }

    [[nodiscard]] std::optional<hd_audio_binding> inspect_binding(
        const dualsense_usb_candidate& candidate) {
        const auto metadata = session_->query_metadata(candidate.target);
        if (!metadata.has_value()) {
            return std::nullopt;
        }
        return qualify_hd_audio_binding(*metadata, candidate);
    }

    [[nodiscard]] bool handle_reconnect_start(
        const std::optional<dualsense_usb_candidate>& candidate,
        const maintenance_time now) {
        start_clicked_ = true;
        if (!candidate.has_value() || !binding_.has_value() || !original_target_.has_value() ||
            !window_.is_foreground() || !session_->focus_is_current_foreground()) {
            window_.set_status("Start ignored. Reconnect the same USB controller and keep this "
                               "window foreground.");
            output_
                << "dualsense_hd_reconnect_start=ignored reason=foreground_or_device_not_ready\n";
            output_.flush();
            return true;
        }
        const auto previous_target = original_target_.value();
        const auto expected_binding = binding_.value();
        if (candidate->target == previous_target) {
            set_step_status("Unplug the USB cable, reconnect the same controller, "
                            "then click Start sequence.");
            output_ << "dualsense_hd_reconnect_start=ignored reason=target_not_recreated target="
                    << format_rumble_target(candidate->target) << '\n';
            output_.flush();
            return true;
        }
        const auto new_binding = inspect_binding(*candidate);
        if (!new_binding.has_value() || new_binding.value() != expected_binding) {
            window_.set_status("Start ignored. Reconnect the same USB controller.");
            output_ << "dualsense_hd_reconnect_start=ignored reason=physical_identity_mismatch "
                    << "target=" << format_rumble_target(candidate->target) << '\n';
            output_.flush();
            return true;
        }
        if (!session_->update_targeted_output(now, candidate->target)) {
            incomplete_reason_ = "reconnect_output_synchronize_failed";
            return false;
        }
        if (!session_->retarget_hd_audio(candidate->target)) {
            incomplete_reason_ = "audio_service_retarget_failed";
            return false;
        }
        const auto opened = step_runner_->open(candidate->target);
        if (!opened.has_value()) {
            copy_step_failure();
            return false;
        }
        selected_ = candidate;
        awaiting_reconnect_ = false;
        awaiting_audio_ready_ = true;
        audio_readiness_deadline_ = diagnostic_clock::now() + step_evidence_limit;
        window_.set_running(true);
        set_step_status("Reconnected. Preparing the effect.");
        output_ << "dualsense_hd_reconnect_start=accepted old_target="
                << format_rumble_target(previous_target)
                << " new_target=" << format_rumble_target(candidate->target)
                << " haptic_endpoint_match=true container_match=true fresh_context="
                << opened->identity.id.value << " previous_clip_reused=false\n";
        output_.flush();
        return true;
    }

    [[nodiscard]] bool wait_for_audio_readiness(const diagnostic_clock::time_point now) {
        if (!selected_.has_value() || step_runner_ == nullptr || !step_runner_->has_context()) {
            incomplete_reason_ = "audio_wait_context_missing";
            return false;
        }
        const auto target = selected_.value().target;
        const auto backend = step_runner_->current_backend(target);
        if (!backend.has_value()) {
            step_runner_->report_progress(now, target, std::nullopt);
            set_step_status("Waiting for the USB controller.");
            return true;
        }
        const auto audio = ready_audio(*session_, target, backend->backend);
        if (!audio.has_value()) {
            if (now < audio_readiness_deadline_) {
                step_runner_->report_progress(now, target, audio);
                set_step_status("Preparing the effect.");
                return true;
            }
            step_runner_->report_progress(now, target, audio);
            awaiting_audio_ready_ = false;
            output_ << "dualsense_hd_audio_fallback=readiness_deadline target="
                    << format_rumble_target(target)
                    << " route_preference=prefer_hd fallback=classic rumble=low_level\n";
            output_.flush();
            return begin_step(hd_audio_steps()[sequence_.index()], *backend, std::nullopt, now);
        }
        if (!audio->audio.has_value()) {
            return true;
        }
        const auto ready_progress = audio.value();
        const auto audio_instance = ready_progress.audio.value();
        step_runner_->report_progress(now, target, audio);
        awaiting_audio_ready_ = false;
        output_ << "dualsense_hd_audio_ready target=" << format_rumble_target(target)
                << " backend=" << backend->backend.incarnation.id.value
                << " audio_id=" << audio_instance.id.value
                << " transport_open=true stream_started=true ready=true\n";
        output_.flush();
        return begin_step(hd_audio_steps()[sequence_.index()], *backend, ready_progress, now);
    }

    [[nodiscard]] bool begin_step(const hd_audio_step& step,
        const output::backend_progress& backend,
        const std::optional<audio_detail::audio_target_progress>& audio,
        const diagnostic_clock::time_point now) {
        if (step_runner_ == nullptr ||
            !step_runner_->begin(step, sequence_.index() + 1U, backend, audio, now)) {
            copy_step_failure();
            return false;
        }
        sequence_.mark_pending();
        window_.disable_confirmation();
        set_step_status(std::string{step.instruction} + " Preparing effect...");
        return true;
    }

    [[nodiscard]] bool advance_active_step(const diagnostic_clock::time_point now,
        const connection_target target) {
        if (step_runner_ == nullptr || !step_runner_->has_active_step()) {
            return true;
        }
        const auto progress = step_runner_->current_backend(target);
        if (!progress.has_value() || !step_runner_->active_backend_is_current(*progress)) {
            incomplete_reason_ = "backend_retired_during_step";
            return false;
        }
        if (const auto confirmation = window_.take_step_result(); confirmation.has_value()) {
            return accept_user_result(*confirmation) && !sequence_.complete();
        }
        if (sequence_.awaiting_confirmation(now)) {
            return true;
        }
        const auto state = step_runner_->observe(*progress, now);
        if (state == hd_audio_step_state::failed) {
            copy_step_failure();
            return false;
        }
        if (state == hd_audio_step_state::waiting) {
            return true;
        }
        return enable_confirmation();
    }

    [[nodiscard]] bool enable_confirmation() {
        if (step_runner_ == nullptr || !step_runner_->has_active_step()) {
            return false;
        }
        const auto readiness = sequence_.mark_ready();
        if (!readiness.has_value()) {
            incomplete_reason_ = "manual_confirmation_identity_exhausted";
            return false;
        }
        std::string_view instruction;
        switch (step_runner_->confirmation_prompt()) {
        case hd_audio_confirmation_prompt::hd_pulse:
            instruction = "Check the half-second HD pulse in both grips.";
            break;
        case hd_audio_confirmation_prompt::fallback_pulse:
            instruction = "HD is unavailable. Check the ordinary vibration.";
            break;
        case hd_audio_confirmation_prompt::classic_pulse:
            instruction = "Check the brief ordinary vibration.";
            break;
        case hd_audio_confirmation_prompt::output_off:
            instruction = "Check that the controller is quiet.";
            break;
        }
        set_step_status(std::string{instruction} + " Choose Works or Does not work when ready.");
        window_.enable_confirmation(*readiness);
        return true;
    }

    [[nodiscard]] bool accept_user_result(const manual_step_confirmation& confirmation) {
        if (step_runner_ == nullptr || !step_runner_->has_active_step()) {
            return true;
        }
        if (!sequence_.record_result(confirmation, diagnostic_clock::now())) {
            return true;
        }
        const auto completed_action = step_runner_->complete(confirmation);
        if (!completed_action.has_value()) {
            incomplete_reason_ = "active_step_completion_failed";
            return false;
        }
        window_.disable_confirmation();
        if (*completed_action == hd_audio_step_action::output_off) {
            close_body_context();
            if (!incomplete_reason_.empty()) {
                return false;
            }
            selected_.reset();
            awaiting_reconnect_ = true;
            window_.set_running(false);
            set_step_status("Unplug the USB cable, reconnect the same controller, "
                            "then click Start sequence.");
            output_ << "dualsense_hd_reconnect=awaiting_user_disconnect_and_new_start "
                       "automatic_replay=false old_lease_closed=true\n";
            output_.flush();
            next_readiness_report_ = diagnostic_clock::now();
            return true;
        }
        if (sequence_.complete()) {
            close_body_context();
            return incomplete_reason_.empty();
        }
        window_.set_status("Preparing the next effect. Keep this window in the foreground.");
        return true;
    }

    void copy_step_failure() {
        if (incomplete_reason_.empty() && step_runner_ != nullptr &&
            !step_runner_->failure_reason().empty()) {
            incomplete_reason_ = step_runner_->failure_reason();
        }
    }

    void close_body_context() {
        if (step_runner_ != nullptr) {
            static_cast<void>(step_runner_->close());
            copy_step_failure();
        }
    }

    void cleanup_body_context() {
        window_.disable_confirmation();
        if (step_runner_ != nullptr) {
            step_runner_->cleanup();
            copy_step_failure();
        }
    }

    [[nodiscard]] int finish() {
        if (!sequence_started_) {
            if (incomplete_reason_.empty()) {
                if (start_clicked_) {
                    incomplete_reason_ = "start_not_accepted";
                } else if (window_.abort_requested()) {
                    incomplete_reason_ = "user_abort";
                } else {
                    incomplete_reason_ = "user_start_not_clicked";
                }
            }
            output_ << "dualsense_hd_sequence=not_started reason=" << incomplete_reason_ << '\n';
        }
        if (step_runner_ != nullptr && step_runner_->has_context()) {
            cleanup_body_context();
        }
        window_.set_running(false);
        if (!sequence_.complete()) {
            window_.set_status("Sequence incomplete. See the diagnostic report.");
        } else if (sequence_.failed() == 0U) {
            window_.set_status("Sequence complete. Every effect was reported as working.");
        } else {
            window_.set_status(
                "Sequence complete. One or more effects were reported as not working.");
        }
        const auto stop = session_->shutdown();
        report_shutdown(stop);
        const bool drained = runtime_and_connections_drained(stop);
        const bool complete = sequence_.complete() && drained && incomplete_reason_.empty();
        output_ << "dualsense_hd_sequence=" << (complete ? "complete" : "incomplete")
                << " steps=" << hd_audio_steps().size() << " user_passed=" << sequence_.passed()
                << " user_failed=" << sequence_.failed()
                << " audio_service_stopped_before_connections=true physical_effect=not_measured";
        if (!complete) {
            if (incomplete_reason_.empty()) {
                incomplete_reason_ = drained ? "user_reported_failure" : "cleanup_unverified";
            }
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
            if (step_runner_ != nullptr && step_runner_->has_context()) {
                cleanup_body_context();
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
    manual_sequence sequence_;
    std::optional<dualsense_usb_candidate> selected_;
    std::optional<connection_target> original_target_;
    std::optional<hd_audio_binding> binding_;
    std::unique_ptr<hd_audio_step_runner> step_runner_;
    diagnostic_clock::time_point deadline_;
    diagnostic_clock::time_point next_readiness_report_;
    diagnostic_clock::time_point audio_readiness_deadline_;
    bool start_clicked_{};
    bool sequence_started_{};
    bool awaiting_audio_ready_{};
    bool awaiting_reconnect_{};
    std::string incomplete_reason_;
};

} // namespace

int run_hd_audio_probe_worker(const probe_options& options, std::ostream& output) {
    return hd_audio_probe_run{options, output}.run();
}

} // namespace inputnext::gameinput_detail
