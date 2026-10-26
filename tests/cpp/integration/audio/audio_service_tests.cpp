#include "audio/audio_service.hpp"
#include "audio/controlled_native_port.hpp"
#include "audio/native_types.hpp"
#include "audio_service_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::audio_detail;
using namespace inputnext::audio_integration_test_support;
using namespace inputnext::output;

[[nodiscard]] std::optional<std::size_t> call_index(
    const test::audio::controlled_native_port_state& state,
    const native_operation operation) noexcept {
    const auto count = state.call_count();
    for (std::size_t index = 0U; index < count; ++index) {
        const auto call = state.call_at(index);
        if (call.has_value() && call->operation == operation) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] body_playback_receipt replace_body(audio_service_fixture& fixture,
    const audio_instance audio,
    const std::span<const float> samples) {
    auto context = fixture.authority().open(fixture.graph.target);
    if (!context.has_value()) {
        throw std::runtime_error{"audio service integration context could not open"};
    }
    const auto preparer = fixture.authority().pcm_preparer();
    auto prepared = preparer.prepare(body_pcm_input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    if (!prepared.has_value()) {
        throw std::runtime_error{"audio service integration PCM could not be prepared"};
    }
    const body_request request{.audio = audio,
        .sequence = 1U,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd,
        .fallback = {.low_frequency = 0.25F, .high_frequency = 0.5F}};
    auto submitted = fixture.authority().submit_body(*context,
        request,
        body_playback_update{.command = body_pcm_command::replace,
            .replacement = std::move(*prepared)});
    if (!submitted.has_value()) {
        throw std::runtime_error{"audio service integration body could not be submitted"};
    }
    return *submitted;
}

} // namespace

TEST_CASE("audio.setup.qualifies_native_stream_before_publishing_readiness",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool is_live = fixture.wait_for_live();
    if (!is_live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(is_live);

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    const auto opened = required_value(call_index(*state, native_operation::get_device),
        "native open call was not observed");
    const auto activated =
        required_value(call_index(*state, native_operation::activate_audio_client),
            "native activation call was not observed");
    const auto mix = required_value(call_index(*state, native_operation::get_mix_format),
        "mix format query was not observed");
    const auto supported = required_value(call_index(*state, native_operation::is_format_supported),
        "format support query was not observed");
    const auto initialized = required_value(call_index(*state, native_operation::initialize),
        "stream initialization was not observed");
    const auto render_service =
        required_value(call_index(*state, native_operation::get_render_service),
            "render service acquisition was not observed");
    const auto first_buffer = required_value(call_index(*state, native_operation::get_buffer),
        "native prefill buffer acquisition was not observed");
    const auto first_release = required_value(call_index(*state, native_operation::release_buffer),
        "native prefill buffer release was not observed");
    const auto started = required_value(call_index(*state, native_operation::start),
        "stream start was not observed");
    const auto revalidated =
        required_value(call_index(*state, native_operation::get_endpoint_state),
            "post-start identity revalidation was not observed");

    CHECK(opened < activated);
    CHECK(activated < mix);
    CHECK(mix < supported);
    CHECK(supported < initialized);
    CHECK(initialized < render_service);
    CHECK(render_service < first_buffer);
    CHECK(first_buffer < first_release);
    CHECK(first_release < started);
    CHECK(started < revalidated);
    CHECK(state->count_calls(native_operation::get_device) == 1U);
    CHECK(state->count_calls(native_operation::initialize) == 1U);
    CHECK(state->all_native_calls_on_worker());

    const auto progress = required_value(fixture.service->progress(fixture.graph.target),
        "audio service progress was not published");
    CHECK(progress.ready);
    CHECK(progress.native_stream_started);
    CHECK(progress.mmcss_registered);
}

TEST_CASE("audio.format.rejects_malformed_quad_before_initialize", "[integration][audio]") {
    audio_service_fixture fixture{true};
    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);

    REQUIRE(wait_until([&state] {
        return state->count_calls(native_operation::is_format_supported) == 0U &&
               state->count_calls(native_operation::get_mix_format) > 0U;
    }));
    REQUIRE(fixture.wait_for_residents(0U));
    CHECK(state->count_calls(native_operation::initialize) == 0U);
    CHECK(state->count_calls(native_operation::start) == 0U);
    CHECK(state->count_calls(native_operation::finish_worker_sta) == 1U);
    CHECK_FALSE(fixture.service->progress(fixture.graph.target).has_value());
}

TEST_CASE("audio.format.rejects_a_closest_quad_before_initialize", "[integration][audio]") {
    audio_service_fixture fixture{false, std::nullopt, true};
    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);

    REQUIRE(wait_until([&state] {
        return state->count_calls(native_operation::is_format_supported) == 1U &&
               state->count_calls(native_operation::finish_worker_sta) == 1U;
    }));
    CHECK(state->count_calls(native_operation::initialize) == 0U);
    CHECK(state->count_calls(native_operation::start) == 0U);
    CHECK(state->all_native_calls_on_worker());
}

TEST_CASE("audio.setup.rolls_back_a_partially_initialized_native_stream_on_its_worker",
    "[integration][audio]") {
    audio_service_fixture fixture{false, native_operation::get_render_service};
    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);

    REQUIRE(wait_until([&state] {
        return state->count_calls(native_operation::get_render_service) == 1U &&
               state->count_calls(native_operation::finish_worker_sta) == 1U;
    }));
    CHECK(state->count_calls(native_operation::get_buffer) == 0U);
    CHECK(state->count_calls(native_operation::start) == 0U);
    CHECK(state->all_native_calls_on_worker());
}

TEST_CASE("audio.playback.admits_current_pcm_and_counts_only_successful_submission",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool is_live = fixture.wait_for_live();
    if (!is_live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(is_live);
    const auto audio = fixture.audio_instance();
    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F};
    static_cast<void>(replace_body(fixture, audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(audio));

    const auto state = fixture.native_states.front();
    REQUIRE(state != nullptr);
    state->signal_buffer_event();
    REQUIRE(wait_until([&fixture] {
        const auto current = fixture.service->progress(fixture.graph.target);
        return current.has_value() && current->accepted_pcm_buffers != 0U;
    }));

    const auto progress = required_value(fixture.service->progress(fixture.graph.target),
        "audio service progress disappeared after submission");
    const auto route =
        required_value(progress.last_admitted_route, "current route was not admitted");
    const auto playback =
        required_value(progress.last_admitted_playback, "current playback was not admitted");
    CHECK(route.intent.identity.audio == audio);
    CHECK(playback.valid());
    CHECK(progress.admitted_blocks == 1U);
    CHECK(progress.source_frames_consumed == samples.size());
    CHECK(progress.accepted_pcm_buffers == 1U);
    CHECK(progress.accepted_pcm_frames == 64U);
    CHECK(state->count_calls(native_operation::get_buffer) == 2U);
    CHECK(state->count_calls(native_operation::release_buffer) == 2U);
}

TEST_CASE("audio.lifecycle.closes_before_a_blocked_get_buffer_and_recreates_without_replay",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool is_live = fixture.wait_for_live();
    if (!is_live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(is_live);
    const auto old_audio = fixture.audio_instance();
    constexpr std::array samples{0.25F, 0.5F, 0.75F, -0.25F, 0.125F, 0.375F, -0.5F, -0.75F};
    static_cast<void>(replace_body(fixture, old_audio, samples));
    REQUIRE(fixture.wait_for_hd_ack(old_audio));

    const auto old_state = fixture.native_states.front();
    REQUIRE(old_state != nullptr);
    old_state->block_on(native_operation::get_buffer);
    release_controlled_native_block release_get_buffer{old_state};
    old_state->signal_buffer_event();
    REQUIRE(old_state->wait_until_blocked(native_operation::get_buffer));

    static_cast<void>(fixture.configure_native_port(1U));
    fixture.service->synchronize(connection_inventory{});
    REQUIRE(wait_until([&fixture] {
        const auto row = fixture.service->progress(fixture.graph.target);
        return row.has_value() && !row->ready && row->phase != audio_target_phase::live;
    }));
    CHECK(fixture.service->progress().resident_slots == 1U);
    CHECK(fixture.service->progress().admitted_blocks == 0U);
    release_get_buffer.release();
    REQUIRE(fixture.wait_for_residents(0U));

    const auto inventory = fixture.graph.backing.runtime->connections();
    REQUIRE(inventory.size == 1U);
    fixture.service->synchronize(inventory);
    REQUIRE(wait_until([&fixture, old_audio] {
        const auto row = fixture.service->progress(fixture.graph.target);
        return row.has_value() && row->ready && row->audio.has_value() && *row->audio != old_audio;
    }));
    const auto new_audio = fixture.audio_instance();
    CHECK(new_audio != old_audio);

    const auto new_state = fixture.native_states.back();
    REQUIRE(new_state != nullptr);
    new_state->signal_buffer_event();
    REQUIRE(wait_until(
        [&fixture] { return fixture.service->progress().accepted_silent_buffers > 1U; }));
    CHECK(fixture.service->progress().admitted_blocks == 0U);
    CHECK(fixture.service->progress().source_frames_consumed == 0U);
    CHECK(old_state->count_calls(native_operation::get_buffer) == 2U);
    CHECK(old_state->count_calls(native_operation::release_buffer) == 2U);
}

TEST_CASE("audio.resource.reserves_endpoint_or_container_before_opening_a_conflicting_target",
    "[integration][audio]") {
    audio_service_fixture fixture{};
    const bool is_live = fixture.wait_for_live();
    if (!is_live) {
        FAIL(fixture.readiness_diagnostic());
    }
    REQUIRE(is_live);
    const auto second_state = fixture.configure_native_port(1U);

    fixture.graph.backing.connect(
        gameinput_test_support::handle(fixture.graph.backing.device_memory[1U]));
    fixture.graph.backing.maintain();
    const auto inventory = fixture.graph.backing.runtime->connections();
    REQUIRE(inventory.size == 2U);
    REQUIRE(fixture.graph.connections->synchronize(inventory).has_value());
    fixture.service->synchronize(inventory);
    const auto second_target = inventory.entries[1U].target;
    REQUIRE(wait_until([&fixture, second_target] {
        const auto current = fixture.graph.connections->query_backend_progress(second_target);
        return current.has_value() && current->phase == backend_phase::current;
    }));
    REQUIRE(wait_until([&second_state] {
        return second_state->count_calls(native_operation::finish_worker_sta) == 1U;
    }));
    REQUIRE(fixture.wait_for_residents(1U));

    CHECK(second_state->count_calls(native_operation::begin_worker_sta) == 1U);
    CHECK(second_state->count_calls(native_operation::get_device) == 0U);
    CHECK(second_state->count_calls(native_operation::activate_audio_client) == 0U);
    CHECK(fixture.service->progress().resident_slots == 1U);
    CHECK(fixture.service->progress().live_slots == 1U);
}
