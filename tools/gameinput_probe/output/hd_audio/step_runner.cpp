#include "gameinput_probe/output/hd_audio/step_runner.hpp"

#include "audio/audio_service.hpp"
#include "gameinput_probe/manual/sequence.hpp"
#include "gameinput_probe/output/hd_audio/evidence.hpp"
#include "gameinput_probe/output/hd_audio/report.hpp"
#include "gameinput_probe/output/hd_audio/sequence.hpp"
#include "gameinput_probe/output/rumble/report.hpp"
#include "gameinput_probe/output/session.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

using diagnostic_clock = std::chrono::steady_clock;
constexpr auto step_evidence_limit = std::chrono::seconds{10};
constexpr auto diagnostic_ttl = std::chrono::seconds{5};
constexpr auto output_report_interval = std::chrono::milliseconds{500};

[[nodiscard]] output::native_output expected_native_output(const output::body_route_intent& intent,
    const output::rumble_value fallback) noexcept {
    return output::native_output{.channels = output::supported_channel_mask,
        .values = {.rumble = intent.route == output::body_actuator_route::hd
                                 ? output::rumble_value{}
                                 : fallback},
        .body = intent};
}

} // namespace

std::optional<output::backend_progress> hd_audio_step_runner::current_backend(
    const connection_target target) const {
    auto progress = session_.backend_progress(target);
    if (!progress.has_value() || progress->backend.target != target ||
        progress->phase != output::backend_phase::current ||
        progress->represented_channels != output::supported_channel_mask ||
        progress->unsupported_channels != 0U) {
        return std::nullopt;
    }
    return progress;
}

hd_audio_step_runner::hd_audio_step_runner(output_session& session, std::ostream& output) noexcept
    : session_(session),
      output_(output) {
}

std::optional<output::public_context> hd_audio_step_runner::open(const connection_target target) {
    if (context_.has_value()) {
        fail("body_context_already_open");
        return std::nullopt;
    }
    const auto opened = session_.open_body_context(target);
    if (!opened.has_value()) {
        fail("public_body_context_open_failed");
        return std::nullopt;
    }
    context_ = *opened;
    last_receipt_.reset();
    body_sequence_ = 0U;
    failure_reason_ = {};
    return context_;
}

bool hd_audio_step_runner::has_context() const noexcept {
    return context_.has_value();
}

bool hd_audio_step_runner::has_receipt() const noexcept {
    return last_receipt_.has_value();
}

bool hd_audio_step_runner::has_active_step() const noexcept {
    return active_.has_value();
}

std::string_view hd_audio_step_runner::failure_reason() const noexcept {
    return failure_reason_;
}

bool hd_audio_step_runner::active_backend_is_current(
    const output::backend_progress& backend) const noexcept {
    return active_.has_value() && backend.backend == active_->backend &&
           backend.phase == output::backend_phase::current;
}

std::optional<hd_audio_step_runner::prepared_body_step> hd_audio_step_runner::prepare_body_step(
    const hd_audio_step& step,
    const output::backend_handle& backend,
    const connection_target target,
    const std::optional<audio_detail::audio_target_progress>& audio) {
    const auto previous_receipt = last_receipt_;
    output::audio_instance audio_identity{};
    if (audio.has_value() && audio->audio.has_value()) {
        audio_identity = audio->audio.value();
    }
    if (audio_identity.unbound() && previous_receipt.has_value()) {
        audio_identity = previous_receipt->body.identity.audio;
    }

    const bool continuing_unbound_lease =
        previous_receipt.has_value() && previous_receipt->body.identity.audio.unbound();
    const bool unbound_allowed =
        step.action == hd_audio_step_action::hd_pulse || continuing_unbound_lease;
    const bool invalid_bound_identity =
        !audio_identity.unbound() &&
        (!audio_identity.valid() || audio_identity.target != target ||
            audio_identity.authority != backend.incarnation.owner.authority);
    const bool invalid_unbound_identity = audio_identity.unbound() && !unbound_allowed;
    if (invalid_bound_identity || invalid_unbound_identity) {
        fail("body_audio_identity_missing_or_stale");
        return std::nullopt;
    }

    output::body_playback_update playback{.command = step.pcm_command, .replacement = {}};
    if (step.pcm_command == output::body_pcm_command::replace) {
        const auto samples = make_hd_audio_pulse();
        const auto prepared = session_.prepare_body_pcm(samples, hd_audio_pulse_gain());
        if (!prepared.has_value()) {
            fail("pcm_prepare_failed");
            output_ << "dualsense_hd_step=submit_failed name=" << step.name
                    << " phase=prepare code=" << static_cast<unsigned int>(prepared.error())
                    << '\n';
            output_.flush();
            return std::nullopt;
        }
        playback.replacement = *prepared;
    }

    auto existing_lease = output::body_lease_id{};
    if (previous_receipt.has_value() && previous_receipt->body.identity.audio == audio_identity) {
        existing_lease = previous_receipt->body.identity.lease;
    }
    const output::body_request request{.lease = existing_lease,
        .audio = audio_identity,
        .sequence = existing_lease.valid() ? body_sequence_ + 1U : 1U,
        .ttl = diagnostic_ttl,
        .preference = step.preference,
        .fallback = step.fallback};
    return prepared_body_step{.audio = audio_identity,
        .request = request,
        .playback = std::move(playback)};
}

bool hd_audio_step_runner::begin(const hd_audio_step& step,
    const std::size_t step_number,
    const output::backend_progress& backend,
    const std::optional<audio_detail::audio_target_progress>& audio,
    const diagnostic_clock::time_point now) {
    if (!context_.has_value()) {
        fail("body_context_missing");
        return false;
    }
    if (active_.has_value()) {
        fail("body_step_already_active");
        return false;
    }
    if (step_number == 0U || step_number > hd_audio_steps().size()) {
        fail("step_index_out_of_range");
        return false;
    }
    if (body_sequence_ == UINT64_MAX) {
        fail("body_sequence_exhausted");
        return false;
    }

    const auto context = context_.value();
    const auto target = context.identity.target;
    const auto prepared = prepare_body_step(step, backend.backend, target, audio);
    if (!prepared.has_value()) {
        return false;
    }

    const auto& submission = *prepared;
    const auto before = current_backend(target);
    if (!before.has_value() || before->backend != backend.backend) {
        fail("backend_changed_before_body_submission");
        return false;
    }
    const auto audio_before = session_.hd_audio_progress(target);
    auto request = submission.request;
    auto submitted = session_.submit_body_pcm(context, request, submission.playback);
    // A stale lease is rejected before commit; acquire one fresh lease without replaying a clip.
    if (!submitted.has_value() && submitted.error() == output::error::stale_lease &&
        request.lease.valid()) {
        request.lease = {};
        request.sequence = 1U;
        submitted = session_.submit_body_pcm(context, request, submission.playback);
        output_ << "dualsense_hd_lease_recovery=stale_lease fresh_acquisition=true "
                   "first_submit_rejected=true retry_count=1"
                << " retry_accepted=" << std::boolalpha << submitted.has_value() << '\n';
        output_.flush();
    }
    if (!submitted.has_value()) {
        fail("body_submission_failed");
        output_ << "dualsense_hd_step=submit_failed name=" << step.name
                << " phase=authority code=" << static_cast<unsigned int>(submitted.error()) << '\n';
        output_.flush();
        return false;
    }

    body_sequence_ = request.sequence;
    last_receipt_ = *submitted;
    const bool audio_baseline_matches = audio_before.has_value() &&
                                        audio_before->backend == backend.backend &&
                                        audio_before->audio == submission.audio;
    const auto source = audio_baseline_matches ? audio_before->source_frames_consumed : 0U;
    const auto pcm_buffers = audio_baseline_matches ? audio_before->accepted_pcm_buffers : 0U;
    const auto pcm_frames = audio_baseline_matches ? audio_before->accepted_pcm_frames : 0U;
    active_ = active_step{.definition = &step,
        .target = target,
        .backend = backend.backend,
        .audio = submission.audio,
        .receipt = *submitted,
        .expected_body = {.intent = submitted->body.intent,
            .accepted_sequence = submitted->body.accepted_sequence,
            .expires_at = now + diagnostic_ttl,
            .fallback_rumble = request.fallback},
        .expected_output = expected_native_output(submitted->body.intent, request.fallback),
        .before_attempt = before->last_attempt,
        .previous_pcm_buffers = pcm_buffers,
        .previous_pcm_frames = pcm_frames,
        .previous_source_frames = source,
        .evidence_deadline = now + step_evidence_limit,
        .next_report = now};
    active_step_number_ = step_number;
    output_ << "dualsense_hd_step_submitted name=" << step.name
            << " target=" << format_rumble_target(target)
            << " backend=" << backend.backend.incarnation.id.value
            << " audio_id=" << submission.audio.id.value
            << " accepted_sequence=" << submitted->body.accepted_sequence
            << " route=" << static_cast<unsigned int>(submitted->body.intent.route)
            << " route_revision=" << submitted->body.intent.revision
            << " lease=" << submitted->body.identity.lease.value << " clip_revision=";
    if (submitted->playback.has_value()) {
        output_ << submitted->playback->clip_revision;
    } else {
        output_ << "none";
    }
    output_ << " pcm_frames=" << hd_audio_pulse_frames()
            << " fallback_rumble_low=" << request.fallback.low_frequency
            << " fallback_rumble_high=" << request.fallback.high_frequency << '\n';
    output_.flush();
    return true;
}

hd_audio_step_state hd_audio_step_runner::observe(const output::backend_progress& backend,
    const diagnostic_clock::time_point now) {
    if (!active_.has_value()) {
        fail("active_step_missing");
        return hd_audio_step_state::failed;
    }
    const auto current = *active_;
    if (!active_backend_is_current(backend)) {
        fail("backend_retired_during_step");
        return hd_audio_step_state::failed;
    }

    const auto logical = session_.logical_snapshot(current.target);
    if (!logical.has_value()) {
        if (now >= current.evidence_deadline) {
            report_active(hd_usb_evidence::pending, std::nullopt);
            fail("logical_snapshot_timeout");
            return hd_audio_step_state::failed;
        }
        return hd_audio_step_state::waiting;
    }

    auto updated = current;
    hd_usb_match matched{};
    const auto evidence = assess_hd_usb_evidence(current.backend,
        current.expected_body,
        current.expected_output,
        current.before_attempt,
        backend,
        *logical,
        previous_match_,
        matched);
    updated.usb_evidence = evidence;
    if (evidence == hd_usb_evidence::rejected) {
        active_ = updated;
        report_active(evidence, std::nullopt);
        fail("native_output_rejected_or_identity_changed");
        return hd_audio_step_state::failed;
    }
    if (evidence != hd_usb_evidence::accepted) {
        active_ = updated;
        report_pending_progress(now, updated);
        return wait_for_evidence(now, updated) ? hd_audio_step_state::waiting
                                               : hd_audio_step_state::failed;
    }

    previous_match_ = matched;
    updated.match = matched;
    if (current.receipt.body.intent.route == output::body_actuator_route::hd) {
        updated.pcm_accepted = observe_accepted_pcm(now, current, backend);
        if (!updated.pcm_accepted) {
            active_ = updated;
            return wait_for_evidence(now, updated) ? hd_audio_step_state::waiting
                                                   : hd_audio_step_state::failed;
        }
    } else if (current.definition->action == hd_audio_step_action::hd_pulse) {
        updated.fallback_observed = true;
    }
    active_ = updated;
    report_active(evidence, matched);
    return hd_audio_step_state::ready;
}

bool hd_audio_step_runner::observe_accepted_pcm(const diagnostic_clock::time_point now,
    const active_step& current,
    const output::backend_progress& backend) {
    if (!current.receipt.playback.has_value() || !backend.acknowledged_body_route.has_value()) {
        return false;
    }
    const auto audio = session_.hd_audio_progress(current.target);
    if (!audio.has_value()) {
        return false;
    }
    const bool accepted = accepted_hd_pcm_progress(current.backend,
        current.audio,
        *backend.acknowledged_body_route,
        *current.receipt.playback,
        *audio,
        current.previous_pcm_buffers,
        current.previous_pcm_frames,
        current.previous_source_frames,
        hd_audio_pulse_frames());
    report_progress(now, current.target, audio);
    return accepted;
}

void hd_audio_step_runner::report_pending_progress(const diagnostic_clock::time_point now,
    const active_step& current) {
    if (now < current.next_report) {
        return;
    }
    auto updated = current;
    updated.next_report = now + output_report_interval;
    active_ = updated;
    report_progress(now, current.target, session_.hd_audio_progress(current.target));
}

bool hd_audio_step_runner::wait_for_evidence(const diagnostic_clock::time_point now,
    const active_step& current) {
    if (now < current.evidence_deadline) {
        return true;
    }
    const bool hd_route = current.receipt.body.intent.route == output::body_actuator_route::hd;
    report_active(current.usb_evidence, current.match);
    fail(hd_route ? "accepted_route_or_pcm_progress_timeout" : "native_setter_evidence_timeout");
    return false;
}

void hd_audio_step_runner::report_active(const hd_usb_evidence evidence,
    const std::optional<hd_usb_match>& match) {
    if (!active_.has_value() || active_->report_emitted || !context_.has_value()) {
        return;
    }
    const auto current = *active_;
    const auto context = context_->identity;
    active_->report_emitted = true;
    write_hd_audio_step(output_,
        current.definition->name,
        current.target,
        active_step_number_,
        hd_audio_steps().size(),
        context,
        current.receipt,
        evidence,
        match,
        current.pcm_accepted,
        current.fallback_observed);
    output_.flush();
}

void hd_audio_step_runner::report_progress(const diagnostic_clock::time_point now,
    const connection_target target,
    const std::optional<audio_detail::audio_target_progress>& audio) {
    if (now < next_output_report_) {
        return;
    }
    write_hd_audio_progress(output_, target, audio, session_.backend_progress(target));
    output_.flush();
    next_output_report_ = now + output_report_interval;
}

hd_audio_confirmation_prompt hd_audio_step_runner::confirmation_prompt() const noexcept {
    if (!active_.has_value()) {
        return hd_audio_confirmation_prompt::classic_pulse;
    }
    const auto& current = *active_;
    if (current.receipt.body.intent.route == output::body_actuator_route::hd &&
        current.pcm_accepted) {
        return hd_audio_confirmation_prompt::hd_pulse;
    }
    if (current.definition->action == hd_audio_step_action::hd_pulse) {
        return hd_audio_confirmation_prompt::fallback_pulse;
    }
    if (current.definition->action == hd_audio_step_action::output_off) {
        return hd_audio_confirmation_prompt::output_off;
    }
    return hd_audio_confirmation_prompt::classic_pulse;
}

std::optional<hd_audio_step_action> hd_audio_step_runner::complete(
    const manual_step_confirmation& confirmation) {
    if (!active_.has_value()) {
        return std::nullopt;
    }
    const auto action = active_->definition->action;
    output_ << "dualsense_hd_user_result step=" << active_->definition->name << " result="
            << (confirmation.result == manual_step_result::passed ? "passed" : "failed")
            << " physical_effect=not_measured\n";
    output_.flush();
    active_.reset();
    return action;
}

bool hd_audio_step_runner::close() {
    if (!context_.has_value()) {
        return failure_reason_.empty();
    }
    const auto context = *context_;
    const auto closed = session_.close_body_context(context);
    bool acknowledged = false;
    if (closed.has_value()) {
        acknowledged = session_.acknowledge_body_context_close(*closed).has_value();
    }
    output_ << "dualsense_hd_context_close target=" << format_rumble_target(context.identity.target)
            << " context=" << context.identity.id.value << " close_submitted=" << std::boolalpha
            << closed.has_value() << " acknowledgement=" << acknowledged << '\n';
    if ((!closed.has_value() || !acknowledged) && failure_reason_.empty()) {
        fail("body_context_close_unacknowledged");
    }
    context_.reset();
    last_receipt_.reset();
    body_sequence_ = 0U;
    output_.flush();
    return closed.has_value() && acknowledged;
}

void hd_audio_step_runner::cleanup() {
    if (active_.has_value() && !active_->report_emitted) {
        report_active(hd_usb_evidence::pending, std::nullopt);
    }
    active_.reset();
    static_cast<void>(close());
}

void hd_audio_step_runner::fail(const std::string_view reason) noexcept {
    if (failure_reason_.empty()) {
        failure_reason_ = reason;
    }
}

} // namespace inputnext::gameinput_detail
