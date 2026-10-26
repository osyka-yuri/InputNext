#include "audio/audio_service.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "audio_service_test_support.hpp"
#include "output/controlled_native_sink.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::audio_detail;
using namespace inputnext::audio_integration_test_support;
using namespace inputnext::output;

[[nodiscard]] body_playback_receipt submit_body(audio_service_fixture& fixture,
    const public_context context,
    const audio_instance audio,
    const std::span<const float> samples,
    const std::uint64_t sequence = 1U,
    const body_lease_id lease = {}) {
    const auto preparer = fixture.authority().pcm_preparer();
    auto prepared = preparer.prepare(body_pcm_input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    if (!prepared.has_value()) {
        throw std::runtime_error{"audio submission fixture could not prepare PCM"};
    }
    auto submitted = fixture.authority().submit_body(context,
        body_request{.lease = lease,
            .audio = audio,
            .sequence = sequence,
            .ttl = 5s,
            .preference = body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.25F, .high_frequency = 0.5F}},
        body_playback_update{.command = body_pcm_command::replace,
            .replacement = std::move(*prepared)});
    if (!submitted.has_value()) {
        throw std::runtime_error{"audio submission fixture could not submit PCM"};
    }
    return *submitted;
}

[[nodiscard]] bool wait_for_target_progress(const audio_service_fixture& fixture,
    const auto& predicate) {
    return wait_until([&fixture, &predicate] {
        const auto current = fixture.service->progress(fixture.graph.target);
        return current.has_value() && predicate(*current);
    });
}

} // namespace

TEST_CASE("audio.submission.retains_admitted_pcm_owner_until_release_buffer_returns",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(live);

    const auto audio = fixture.audio_instance();
    const auto context = required_value(fixture.authority().open(fixture.graph.target),
        "audio submission fixture could not open authority context");
    constexpr std::array original_samples{0.25F, 0.5F, 0.75F, -0.25F};
    constexpr std::array replacement_samples{0.75F, 0.5F, 0.25F, -0.5F, 0.125F, -0.125F};
    const auto original = submit_body(fixture, context, audio, original_samples);
    REQUIRE(fixture.wait_for_hd_ack(audio));

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    state->block_on(native_operation::release_buffer);
    release_controlled_native_block release_buffer{state};
    state->signal_buffer_event();
    REQUIRE(state->wait_until_blocked(native_operation::release_buffer));

    const auto replacement =
        submit_body(fixture, context, audio, replacement_samples, 2U, original.body.identity.lease);
    CHECK(replacement.body.identity.lease == original.body.identity.lease);
    fixture.authority().collect_body_pcm();
    CHECK(fixture.authority().pcm_preparer().usage().buffers == 2U);

    constexpr auto original_frame_count = original_samples.size();
    REQUIRE(wait_for_target_progress(fixture, [](const audio_target_progress& current) {
        return current.admitted_blocks == 1U &&
               current.source_frames_consumed == original_frame_count;
    }));

    const auto blocked = required_value(fixture.service->progress(fixture.graph.target),
        "audio submission progress disappeared while ReleaseBuffer was blocked");
    CHECK(blocked.admitted_blocks == 1U);
    CHECK(blocked.source_frames_consumed == original_samples.size());
    CHECK(blocked.accepted_pcm_buffers == 0U);
    CHECK(blocked.accepted_pcm_frames == 0U);

    release_buffer.release();
    REQUIRE(wait_for_target_progress(fixture,
        [](const audio_target_progress& current) { return current.accepted_pcm_buffers == 1U; }));
    REQUIRE(wait_until([&fixture] {
        fixture.authority().collect_body_pcm();
        return fixture.authority().pcm_preparer().usage().buffers == 1U;
    }));
}

TEST_CASE("audio.submission.release_buffer_failure_keeps_consumption_and_rejects_pcm_acceptance",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(live);

    constexpr std::array samples{0.125F, 0.25F, 0.375F, 0.5F, -0.25F};
    const auto audio = fixture.audio_instance();
    const auto context = required_value(fixture.authority().open(fixture.graph.target),
        "audio submission fixture could not open authority context");
    static_cast<void>(submit_body(fixture, context, audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(audio));

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    state->fail_on(native_operation::release_buffer, 19U);
    state->signal_buffer_event();
    REQUIRE(wait_until(
        [&state] { return state->count_calls(native_operation::finish_worker_sta) == 1U; }));
    REQUIRE(fixture.wait_for_residents(0U));

    const auto progress = fixture.service->progress();
    CHECK(progress.admitted_blocks == 1U);
    CHECK(progress.source_frames_consumed == samples.size());
    CHECK(progress.accepted_pcm_buffers == 0U);
    CHECK(progress.accepted_pcm_frames == 0U);
    CHECK(state->count_calls(native_operation::get_buffer) == 2U);
    CHECK(state->count_calls(native_operation::release_buffer) == 2U);
    CHECK(state->all_native_calls_on_worker());
}

TEST_CASE("audio.submission.global_close_during_get_buffer_submits_only_silence",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(live);

    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
    const auto audio = fixture.audio_instance();
    const auto context = required_value(fixture.authority().open(fixture.graph.target),
        "audio submission fixture could not open authority context");
    static_cast<void>(submit_body(fixture, context, audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(audio));

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    state->block_on(native_operation::get_buffer);
    release_controlled_native_block release_get_buffer{state};
    state->signal_buffer_event();
    REQUIRE(state->wait_until_blocked(native_operation::get_buffer));

    fixture.service->close_admission();
    REQUIRE(wait_for_target_progress(fixture,
        [](const audio_target_progress& current) { return !current.ready; }));
    release_get_buffer.release();
    REQUIRE(fixture.wait_for_residents(0U));

    const auto progress = fixture.service->progress();
    CHECK(progress.admitted_blocks == 0U);
    CHECK(progress.source_frames_consumed == 0U);
    CHECK(progress.accepted_pcm_buffers == 0U);
    CHECK(progress.accepted_pcm_frames == 0U);
    CHECK(progress.accepted_silent_buffers >= 2U);
    CHECK(state->count_calls(native_operation::get_buffer) == 2U);
    CHECK(state->count_calls(native_operation::release_buffer) == 2U);
}

TEST_CASE("audio.lifecycle.backend_replacement_closes_frozen_backend_without_replaying_clip",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool live = fixture.wait_for_live();
    if (!live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(live);

    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F, 0.125F, -0.5F};
    const auto old_audio = fixture.audio_instance();
    const auto context = required_value(fixture.authority().open(fixture.graph.target),
        "audio submission fixture could not open authority context");
    static_cast<void>(submit_body(fixture, context, old_audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(old_audio));
    const auto old_row = required_value(fixture.service->progress(fixture.graph.target),
        "audio slot progress disappeared before backend replacement");
    const auto old_backend =
        required_value(old_row.backend, "audio slot had no frozen backend before replacement");

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    state->block_on(native_operation::get_buffer);
    release_controlled_native_block release_get_buffer{state};
    state->signal_buffer_event();
    REQUIRE(state->wait_until_blocked(native_operation::get_buffer));

    const auto replacement_state = std::make_shared<test::output::controlled_native_state>();
    std::unique_ptr<native_sink> candidate =
        std::make_unique<test::output::controlled_native_sink>(replacement_state);
    const auto replacement = fixture.graph.output_executor->replace_backend(old_backend,
        fixture.graph.target,
        candidate,
        rumble_mask,
        body_route_support::hd);
    REQUIRE(replacement.has_value());
    REQUIRE(wait_for_target_progress(fixture,
        [](const audio_target_progress& current) { return !current.ready; }));
    const auto closing = required_value(fixture.service->progress(fixture.graph.target),
        "audio slot progress disappeared before blocked GetBuffer returned");
    CHECK(closing.backend == old_backend);

    release_get_buffer.release();
    REQUIRE(fixture.wait_for_residents(0U));
    const auto backend_progress = fixture.graph.output_executor->progress(*replacement);
    REQUIRE(backend_progress.has_value());
    if (backend_progress->acknowledged_body_route.has_value()) {
        const auto qualified = fixture.graph.output_executor->qualifies_route(
            *backend_progress->acknowledged_body_route);
        REQUIRE(qualified.has_value());
        CHECK_FALSE(*qualified);
        CHECK(backend_progress->acknowledged_body_route->intent.identity.audio != old_audio);
    }
    CHECK(fixture.service->progress().resident_slots == 0U);
    CHECK(state->count_calls(native_operation::get_buffer) == 2U);
    CHECK(state->count_calls(native_operation::release_buffer) == 2U);
}
