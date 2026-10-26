#include "body_playback_fixture.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/playback_test_support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace {

using namespace inputnext::output;
using inputnext::test::output::body_playback_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::shaped_sample;
using inputnext::test::output::take_optional_value;

class permit_release_guard final {
public:
    explicit permit_release_guard(std::optional<body_render_permit>& permit) noexcept
        : permit_(permit) {}

    permit_release_guard(const permit_release_guard&) = delete;
    permit_release_guard& operator=(const permit_release_guard&) = delete;
    permit_release_guard(permit_release_guard&&) = delete;
    permit_release_guard& operator=(permit_release_guard&&) = delete;

    ~permit_release_guard() noexcept { permit_.reset(); }

private:
    std::optional<body_render_permit>& permit_;
};

TEST_CASE("output.body_playback_keep_preserves_cursor_and_completion_is_terminal",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.25F, -0.75F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());
    auto ack_before_keep = fixture.acknowledgement();
    REQUIRE(ack_before_keep.has_value());
    const auto acknowledgement = take_optional_value(ack_before_keep);

    auto first_permit = fixture.admit(identity);
    REQUIRE(first_permit.has_value());
    auto first = take_optional_value(first_permit.value());
    std::array<float, 4U> first_output{};
    const auto first_render = std::move(first).render(first_output);
    CHECK(first_render.source_frames_consumed == 1U);
    CHECK_FALSE(first_render.completed);
    CHECK(first_output[0U] == 0.0F);
    CHECK(first_output[1U] == 0.0F);
    CHECK(first_output[2U] == Catch::Approx(shaped_sample(samples[0U])));

    auto kept = fixture.authority.owner.submit_body(fixture.context, fixture.request(2U));
    REQUIRE(kept.has_value());
    fixture.body = *kept;
    const auto snapshot =
        fixture.authority.owner.read_snapshot(fixture.authority.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(fixture.engine->engine().publish(fixture.backend, *snapshot).has_value());
    CHECK_FALSE(fixture.engine->engine().drive_one(fixture.lane));
    CHECK(fixture.native_state->call_count() == 1U);
    CHECK(fixture.acknowledgement() == acknowledgement);

    auto second_permit = fixture.admit(identity);
    REQUIRE(second_permit.has_value());
    auto second = take_optional_value(second_permit.value());
    std::array<float, 4U> second_output{};
    const auto second_render = std::move(second).render(second_output);
    CHECK(second_render.source_frames_consumed == 1U);
    CHECK(second_render.completed);
    CHECK(second_output[2U] == Catch::Approx(shaped_sample(samples[1U])));
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);

    auto replay = fixture.admit(identity);
    REQUIRE(replay.has_value());
    CHECK_FALSE(replay.value().has_value());
}

TEST_CASE("output.body_playback_retry_keeps_cursor_and_stop_prevents_replay",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.2F, 0.4F, 0.8F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());
    auto first_ack_optional = fixture.acknowledgement();
    REQUIRE(first_ack_optional.has_value());
    const auto first_ack = take_optional_value(first_ack_optional);

    auto first_permit = fixture.admit(identity);
    REQUIRE(first_permit.has_value());
    auto first = take_optional_value(first_permit.value());
    std::array<float, 4U> first_output{};
    CHECK(std::move(first).render(first_output).source_frames_consumed == 1U);

    REQUIRE(fixture.engine->engine().request_retry(fixture.backend).has_value());
    auto suspended = fixture.engine->engine().admit_body_render(first_ack, identity, 1U);
    REQUIRE(suspended.has_value());
    CHECK_FALSE(suspended.value().has_value());
    REQUIRE(fixture.engine->engine().drive_one(fixture.lane));
    auto retry_ack_optional = fixture.acknowledgement();
    REQUIRE(retry_ack_optional.has_value());
    const auto retry_ack = take_optional_value(retry_ack_optional);
    CHECK(retry_ack.attempt != first_ack.attempt);

    auto second_permit = fixture.admit(identity);
    REQUIRE(second_permit.has_value());
    auto second = take_optional_value(second_permit.value());
    std::array<float, 4U> second_output{};
    CHECK(std::move(second).render(second_output).source_frames_consumed == 1U);
    CHECK(second_output[2U] == Catch::Approx(shaped_sample(samples[1U])));

    auto stopped = fixture.stop(2U);
    REQUIRE(stopped.has_value());
    CHECK_FALSE(stopped->playback.has_value());
    auto stopped_admission = fixture.admit(identity);
    REQUIRE(stopped_admission.has_value());
    CHECK_FALSE(stopped_admission.value().has_value());
}

TEST_CASE("output.body_playback_same_handle_replacement_rejects_old_clip_identity",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.45F, -0.3F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    const auto& same_handle = prepared.value();
    auto first = fixture.replace(same_handle);
    REQUIRE(first.has_value());
    REQUIRE(first->playback.has_value());
    const auto old_identity = take_optional_value(first->playback);
    REQUIRE(fixture.publish_and_drive());
    auto old_ack_optional = fixture.acknowledgement();
    REQUIRE(old_ack_optional.has_value());
    const auto old_ack = take_optional_value(old_ack_optional);

    auto replacement = fixture.replace(same_handle, 2U);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->playback.has_value());
    const auto replacement_identity = take_optional_value(replacement->playback);
    CHECK(replacement_identity.lease == old_identity.lease);
    CHECK(replacement_identity.clip_revision != old_identity.clip_revision);

    auto old_claim = fixture.engine->engine().admit_body_render(old_ack, old_identity, 1U);
    REQUIRE(old_claim.has_value());
    CHECK_FALSE(old_claim.value().has_value());
    auto new_claim = fixture.engine->engine().admit_body_render(old_ack, replacement_identity, 1U);
    REQUIRE(new_claim.has_value());
    auto new_permit = take_optional_value(new_claim.value());
    std::array<float, 4U> output{};
    CHECK(std::move(new_permit).render(output).source_frames_consumed == 1U);
    CHECK(output[2U] == Catch::Approx(shaped_sample(samples[0U])));
}

TEST_CASE("output.body_playback_audio_loss_retires_clip_until_replace",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.3F, 0.6F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto retired_identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());
    auto old_ack_optional = fixture.acknowledgement();
    REQUIRE(old_ack_optional.has_value());
    const auto old_ack = take_optional_value(old_ack_optional);
    auto held_optional = fixture.admit(retired_identity);
    REQUIRE(held_optional.has_value());
    auto held = take_optional_value(held_optional.value());

    REQUIRE(fixture.authority.trusted.audio_routes().set_ready(fixture.audio, false).has_value());
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);
    auto stale = fixture.engine->engine().admit_body_render(old_ack, retired_identity, 1U);
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale.value().has_value());
    std::array<float, 4U> retired_output{};
    CHECK(std::move(held).render(retired_output).source_frames_consumed == 1U);
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);

    REQUIRE(fixture.authority.trusted.audio_routes().set_ready(fixture.audio, true).has_value());
    auto kept = fixture.keep(2U);
    REQUIRE(kept.has_value());
    CHECK(kept->playback == retired_identity);
    REQUIRE(fixture.publish_and_drive());
    auto resumed_ack_optional = fixture.acknowledgement();
    REQUIRE(resumed_ack_optional.has_value());
    const auto resumed_route_ack = take_optional_value(resumed_ack_optional);
    auto no_revival =
        fixture.engine->engine().admit_body_render(resumed_route_ack, retired_identity, 1U);
    REQUIRE(no_revival.has_value());
    CHECK_FALSE(no_revival.value().has_value());

    auto replacement_samples = fixture.prepare(samples);
    REQUIRE(replacement_samples.has_value());
    auto replaced = fixture.replace(std::move(*replacement_samples), 3U);
    REQUIRE(replaced.has_value());
    REQUIRE(replaced->playback.has_value());
    const auto current_identity = take_optional_value(replaced->playback);
    CHECK(current_identity.clip_revision != retired_identity.clip_revision);
    auto current_ack_optional = fixture.acknowledgement();
    REQUIRE(current_ack_optional.has_value());
    const auto current_ack = take_optional_value(current_ack_optional);
    auto current = fixture.engine->engine().admit_body_render(current_ack, current_identity, 1U);
    REQUIRE(current.has_value());
    const auto current_permit = take_optional_value(current.value());
    CHECK(current_permit.valid());
}

TEST_CASE("output.body_playback_replacement_waits_for_old_target_permit",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array old_samples{0.15F, 0.3F};
    constexpr std::array new_samples{-0.25F, -0.5F};
    auto old_prepared = fixture.prepare(old_samples);
    REQUIRE(old_prepared.has_value());
    auto old_clip = fixture.replace(std::move(*old_prepared));
    REQUIRE(old_clip.has_value());
    REQUIRE(old_clip->playback.has_value());
    const auto old_identity = take_optional_value(old_clip->playback);
    REQUIRE(fixture.publish_and_drive());
    auto acknowledgement_optional = fixture.acknowledgement();
    REQUIRE(acknowledgement_optional.has_value());
    const auto acknowledgement = take_optional_value(acknowledgement_optional);

    auto held = fixture.admit(old_identity);
    REQUIRE(held.has_value());
    auto old_permit = take_optional_value(held.value());

    auto new_prepared = fixture.prepare(new_samples);
    REQUIRE(new_prepared.has_value());
    auto new_clip = fixture.replace(std::move(*new_prepared), 2U);
    REQUIRE(new_clip.has_value());
    REQUIRE(new_clip->playback.has_value());
    const auto new_identity = take_optional_value(new_clip->playback);
    auto blocked = fixture.engine->engine().admit_body_render(acknowledgement, new_identity, 1U);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error() == execution_error::busy);

    std::array<float, 4U> old_output{};
    CHECK(std::move(old_permit).render(old_output).source_frames_consumed == 1U);
    fixture.authority.owner.collect_body_pcm();

    auto admitted_new =
        fixture.engine->engine().admit_body_render(acknowledgement, new_identity, 1U);
    REQUIRE(admitted_new.has_value());
    auto new_permit = take_optional_value(admitted_new.value());
    std::array<float, 4U> new_output{};
    CHECK(std::move(new_permit).render(new_output).source_frames_consumed == 1U);
    CHECK(new_output[2U] == Catch::Approx(shaped_sample(new_samples[0U])));
}

TEST_CASE("output.body_playback_context_close_keeps_held_samples_alive_until_collection",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.35F, 0.55F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());
    auto acknowledgement_optional = fixture.acknowledgement();
    REQUIRE(acknowledgement_optional.has_value());
    const auto acknowledgement = take_optional_value(acknowledgement_optional);

    auto held = fixture.admit(identity);
    REQUIRE(held.has_value());
    auto permit = take_optional_value(held.value());

    const auto closed =
        fixture.authority.owner.close(fixture.context, fixture.context.identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);
    auto denied = fixture.engine->engine().admit_body_render(acknowledgement, identity, 1U);
    REQUIRE(denied.has_value());
    CHECK_FALSE(denied.value().has_value());

    std::array<float, 4U> output{};
    CHECK(std::move(permit).render(output).source_frames_consumed == 1U);
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
}

TEST_CASE("output.body_playback_backend_retirement_drains_held_permit_without_blocking_control",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.2F, 0.7F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());
    auto acknowledgement_optional = fixture.acknowledgement();
    REQUIRE(acknowledgement_optional.has_value());
    const auto acknowledgement = take_optional_value(acknowledgement_optional);

    auto admitted = fixture.admit(identity);
    REQUIRE(admitted.has_value());
    std::atomic<bool> cleanup_finished{};
    std::optional<body_render_permit> held_permit;
    std::jthread cleanup;
    std::future<bool> status_progress;
    std::future<result<mutation_receipt>> control_progress;
    const permit_release_guard release_on_failure{held_permit};
    held_permit.emplace(take_optional_value(admitted.value()));

    const auto retired = fixture.engine->engine().retire(fixture.backend);
    REQUIRE(retired.has_value());
    auto closed_admission =
        fixture.engine->engine().admit_body_render(acknowledgement, identity, 1U);
    REQUIRE(closed_admission.has_value());
    CHECK_FALSE(closed_admission.value().has_value());
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);

    cleanup = std::jthread([&] {
        static_cast<void>(fixture.engine->engine().drive_one(fixture.lane));
        cleanup_finished.store(true, std::memory_order_release);
    });
    status_progress = std::async(std::launch::async, [&fixture] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto progress = fixture.engine->engine().progress(fixture.backend);
            if (progress.has_value() && progress->phase == backend_phase::cleaning) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    });
    const bool status_ready =
        status_progress.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    CHECK(status_ready);
    const bool observed_cleaning = status_ready && status_progress.get();
    CHECK(observed_cleaning);

    control_progress = std::async(std::launch::async, [&fixture] {
        submit_request input_progress{};
        input_progress.selected_channels = leds_mask;
        input_progress.sequence = 1U;
        input_progress.ttl = std::chrono::seconds{1};
        return fixture.authority.owner.submit(fixture.context, input_progress);
    });
    const bool control_ready =
        control_progress.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    CHECK(control_ready);
    CHECK_FALSE(cleanup_finished.load(std::memory_order_acquire));

    std::array<float, 4U> output{};
    auto permit = take_optional_value(held_permit);
    CHECK(std::move(permit).render(output).source_frames_consumed == 1U);
    held_permit.reset();
    cleanup.join();
    CHECK(cleanup_finished.load(std::memory_order_acquire));
    if (control_ready) {
        CHECK(control_progress.get().has_value());
    }
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
    CHECK(fixture.engine->engine().acknowledge_retirement(retired.value()).has_value());
}

TEST_CASE("output.body_render_current_admission_selects_the_fresh_private_clip",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array old_samples{0.1F, 0.2F};
    constexpr std::array current_samples{-0.35F, -0.7F};
    auto old_prepared = fixture.prepare(old_samples);
    REQUIRE(old_prepared.has_value());
    auto old_clip = fixture.replace(std::move(*old_prepared));
    REQUIRE(old_clip.has_value());
    REQUIRE(old_clip->playback.has_value());
    const auto old_identity = take_optional_value(old_clip->playback);
    REQUIRE(fixture.publish_and_drive());

    auto current_prepared = fixture.prepare(current_samples);
    REQUIRE(current_prepared.has_value());
    auto current_clip = fixture.replace(std::move(*current_prepared), 2U);
    REQUIRE(current_clip.has_value());
    REQUIRE(current_clip->playback.has_value());
    const auto current_identity = take_optional_value(current_clip->playback);
    REQUIRE(current_identity.clip_revision != old_identity.clip_revision);
    static_cast<void>(fixture.publish_and_drive());
    REQUIRE(fixture.acknowledgement().has_value());

    auto admitted =
        fixture.engine->engine().admit_current_body_render(fixture.backend, fixture.audio, 1U);
    REQUIRE(admitted.has_value());
    REQUIRE(admitted->has_value());
    auto current = take_optional_value(admitted.value());
    CHECK(current.route.backend == fixture.backend);
    CHECK(current.playback == current_identity);
    std::array<float, 4U> output{};
    const auto rendered = std::move(current.permit).render(output);
    CHECK(rendered.source_frames_consumed == 1U);
    CHECK(output[2U] == Catch::Approx(shaped_sample(current_samples[0U])));
}

TEST_CASE("output.body_render_current_admission_rejects_stale_retry_audio_and_replaced_backend",
    "[integration][output][body_playback]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.25F, 0.5F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    REQUIRE(fixture.publish_and_drive());

    auto invalid_size =
        fixture.engine->engine().admit_current_body_render(fixture.backend, fixture.audio, 0U);
    CHECK((!invalid_size.has_value() && invalid_size.error() == execution_error::invalid_argument));

    auto foreign_backend = fixture.backend;
    ++foreign_backend.incarnation.id.value;
    auto stale_backend =
        fixture.engine->engine().admit_current_body_render(foreign_backend, fixture.audio, 1U);
    CHECK((!stale_backend.has_value() && stale_backend.error() == execution_error::stale_backend));

    auto foreign_audio = fixture.audio;
    ++foreign_audio.id.value;
    auto stale_audio =
        fixture.engine->engine().admit_current_body_render(fixture.backend, foreign_audio, 1U);
    REQUIRE(stale_audio.has_value());
    CHECK_FALSE(stale_audio->has_value());

    REQUIRE(fixture.engine->engine().request_retry(fixture.backend).has_value());
    auto retry_suspended =
        fixture.engine->engine().admit_current_body_render(fixture.backend, fixture.audio, 1U);
    REQUIRE(retry_suspended.has_value());
    CHECK_FALSE(retry_suspended->has_value());
    REQUIRE(fixture.engine->engine().drive_one(fixture.lane));

    const auto original_backend = fixture.backend;
    const auto replacement_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> replacement_sink =
        std::make_unique<controlled_native_sink>(replacement_state);
    auto replacement = fixture.engine->register_backend(fixture.authority.connection.target,
        replacement_sink,
        rumble_mask,
        original_backend,
        body_route_support::hd);
    REQUIRE(replacement.has_value());
    fixture.backend = *replacement;
    const auto replacement_lane = fixture.engine->engine().lane_for(fixture.backend);
    REQUIRE(replacement_lane.has_value());
    fixture.lane = *replacement_lane;
    for (std::size_t attempt = 0U; attempt < 4U && !fixture.acknowledgement().has_value();
        ++attempt) {
        const auto snapshot =
            fixture.authority.owner.read_snapshot(fixture.authority.connection.target);
        REQUIRE(snapshot.has_value());
        REQUIRE(fixture.engine->engine().publish(fixture.backend, *snapshot).has_value());
        static_cast<void>(fixture.engine->engine().drive_one(fixture.lane));
    }
    REQUIRE(fixture.acknowledgement().has_value());

    auto replaced_backend =
        fixture.engine->engine().admit_current_body_render(original_backend, fixture.audio, 1U);
    REQUIRE(replaced_backend.has_value());
    CHECK_FALSE(replaced_backend->has_value());
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);

    REQUIRE(fixture.authority.trusted.audio_routes().set_ready(fixture.audio, false).has_value());
    auto lost_audio =
        fixture.engine->engine().admit_current_body_render(fixture.backend, fixture.audio, 1U);
    REQUIRE(lost_audio.has_value());
    CHECK_FALSE(lost_audio->has_value());
}

TEST_CASE("output.body_render_submission_keeps_voice_and_backend_fenced_until_release",
    "[integration][output][body_playback][concurrency]") {
    body_playback_fixture fixture{};
    constexpr std::array samples{0.2F, 0.7F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    const auto identity = take_optional_value(installed->playback);
    REQUIRE(fixture.publish_and_drive());

    auto admitted = fixture.admit(identity);
    REQUIRE(admitted.has_value());
    auto permit = take_optional_value(admitted.value());
    std::array<float, 4U> output{};
    auto submission = std::move(permit).render_for_submission(output);
    REQUIRE(submission.valid());
    CHECK(submission.result().source_frames_consumed == 1U);
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);

    auto retired = fixture.engine->engine().retire(fixture.backend);
    REQUIRE(retired.has_value());
    std::atomic<bool> cleanup_finished{};
    auto cleanup = std::async(std::launch::async, [&] {
        const bool result = fixture.engine->engine().drive_one(fixture.lane);
        cleanup_finished.store(true, std::memory_order_release);
        return result;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool entered_cleaning = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto progress = fixture.engine->engine().progress(fixture.backend);
        if (progress.has_value() && progress->phase == backend_phase::cleaning) {
            entered_cleaning = true;
            break;
        }
        std::this_thread::yield();
    }
    CHECK(entered_cleaning);
    CHECK_FALSE(cleanup_finished.load(std::memory_order_acquire));
    CHECK(cleanup.wait_for(std::chrono::milliseconds{20}) != std::future_status::ready);

    submission = {};
    CHECK(cleanup.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    if (cleanup.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        CHECK(cleanup.get());
    }
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
    CHECK(fixture.engine->engine().acknowledge_retirement(retired.value()).has_value());
}

} // namespace
