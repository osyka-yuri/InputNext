#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "effects/effects.hpp"
#include "effects_session_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/device_metadata.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::effects_test {
namespace {

using namespace std::chrono_literals;
using effects_integration_test_support::session_fixture;

template <typename Value>
[[nodiscard]] metadata_field<Value> available(const Value& value) {
    return metadata_field<Value>{.availability = metadata_availability::available, .value = value};
}

void configure_dualsense_facts(session_fixture& fixture) {
    fixture.runtime.native->fake->device_facts.vendor_id = available<std::uint16_t>(0x054CU);
    fixture.runtime.native->fake->device_facts.product_id = available<std::uint16_t>(0x0CE6U);
}

[[nodiscard]] bool waits_for_unsupported_target(session_fixture& fixture) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto resolved = fixture.service().resolve(fixture.selector());
        if (!resolved.has_value() && resolved.error() == effects::status::unsupported_target) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

[[nodiscard]] effects::discrete_receipt submit_rumble(session_fixture& fixture,
    const effects::context_handle context,
    const std::uint64_t sequence) {
    constexpr auto ttl = 2s;
    const output::submit_request request{.selected_channels = output::rumble_mask,
        .sequence = sequence,
        .ttl = ttl,
        .values = {.rumble = {.low_frequency = 0.3F, .high_frequency = 0.6F}}};
    const auto submitted = fixture.service().submit(context, request, session_fixture::window(ttl));
    if (!submitted.has_value()) {
        throw std::runtime_error{"Effects progress fixture could not submit rumble"};
    }
    return *submitted;
}

[[nodiscard]] effects::progress wait_for_stage(session_fixture& fixture,
    const effects::publication_revision publication,
    const effects::progress_stage expected) {
    effects::progress last{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto queried = fixture.service().query_progress(fixture.target, publication);
        if (queried.has_value()) {
            last = *queried;
            if (last.stage == expected) {
                return last;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    return last;
}

} // namespace

TEST_CASE("effects.progress_requires_exact_current_representation_and_keeps_eviction_unknown",
    "[integration][effects][progress][projection]") {
    session_fixture fixture;
    fixture.start();
    const auto first_context = fixture.open();
    const auto second_context = fixture.open();

    const auto first = submit_rumble(fixture, first_context, 1U);
    const auto second = submit_rumble(fixture, second_context, 1U);
    CHECK(first.publication != second.publication);

    const auto superseded =
        wait_for_stage(fixture, first.publication, effects::progress_stage::superseded);
    CHECK(superseded.stage == effects::progress_stage::superseded);
    CHECK(superseded.completion == effects::completion_class::none);

    const auto completed =
        wait_for_stage(fixture, second.publication, effects::progress_stage::current_completed);
    CHECK(completed.stage == effects::progress_stage::current_completed);
    CHECK(completed.completion == effects::completion_class::setter_returned_current);
    CHECK(completed.attempt_serial != 0U);

    const auto replacement = submit_rumble(fixture, second_context, 2U);
    const auto evicted = fixture.service().query_progress(fixture.target, second.publication);
    REQUIRE(evicted.has_value());
    CHECK(evicted->stage == effects::progress_stage::unknown);
    CHECK(evicted->completion == effects::completion_class::none);
    CHECK(replacement.publication != second.publication);
}

TEST_CASE("effects.progress_is_scoped_to_the_exact_target_and_fresh_publication",
    "[integration][effects][progress][identity]") {
    session_fixture fixture;
    fixture.start();
    const auto context = fixture.open();
    const auto submitted = submit_rumble(fixture, context, 1U);

    auto unrelated = fixture.target;
    ++unrelated.connection.value;
    const auto hidden = fixture.service().query_progress(unrelated, submitted.publication);
    REQUIRE(hidden.has_value());
    CHECK(hidden->stage == effects::progress_stage::unknown);

    const auto unrelated_revision = fixture.service().query_progress(fixture.target,
        effects::publication_revision{.value = submitted.publication.value + 100U});
    REQUIRE(unrelated_revision.has_value());
    CHECK(unrelated_revision->stage == effects::progress_stage::unknown);
}

TEST_CASE("effects.progress_does_not_complete_an_equal_body_after_another_context_wins",
    "[integration][effects][progress][body][projection]") {
    session_fixture fixture;
    fixture.start();
    const auto first_context = fixture.open();
    const auto second_context = fixture.open();
    constexpr auto ttl = 45s;

    const effects::body_request first_request{.context = first_context,
        .sequence = 1U,
        .ttl = ttl,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.5F},
        .pcm_command = output::body_pcm_command::keep};
    const auto first = fixture.service().submit_body(first_request, session_fixture::window(ttl));
    REQUIRE(first.has_value());

    const effects::body_request second_request{.context = second_context,
        .sequence = 1U,
        .ttl = ttl,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.5F},
        .pcm_command = output::body_pcm_command::keep};
    const auto competing =
        fixture.service().submit_body(second_request, session_fixture::window(ttl));
    REQUIRE(!competing.has_value());
    CHECK(competing.error() == effects::status::busy);
    const effects::body_clear_request clear{.context = first_context,
        .lease_id = first->lease_id,
        .audio_instance_id = first->audio_instance_id,
        .sequence = 2U};
    REQUIRE(fixture.service().clear_body(clear) == effects::status::ok);

    const auto second = fixture.service().submit_body(second_request, session_fixture::window(ttl));
    REQUIRE(second.has_value());
    CHECK(first->lease_id != second->lease_id);
    CHECK(first->publication != second->publication);

    const auto superseded =
        wait_for_stage(fixture, first->publication, effects::progress_stage::superseded);
    CHECK(superseded.stage == effects::progress_stage::superseded);
    CHECK(superseded.completion == effects::completion_class::none);
}

TEST_CASE("effects.capabilities_use_fresh_observation_revisions",
    "[integration][effects][capabilities][metadata]") {
    session_fixture fixture;
    fixture.start();

    const auto first = fixture.service().resolve(fixture.selector());
    const auto second = fixture.service().resolve(fixture.selector());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->target == fixture.target);
    CHECK(second->target == fixture.target);
    CHECK(first->value.observation_revision != 0U);
    CHECK(second->value.observation_revision > first->value.observation_revision);
    CHECK((first->value.supported_channels & output::rumble_mask) == output::rumble_mask);
    CHECK(effects::has_feature(first->value.features, effects::feature_flags::low_rumble));
    CHECK(effects::has_feature(first->value.features, effects::feature_flags::high_rumble));
}

TEST_CASE("effects.capabilities_report_independent_motor_support",
    "[integration][effects][capabilities][hardware]") {
    session_fixture fixture;
    fixture.runtime.native->fake->device_facts.gameinput_capabilities =
        available(native_gameinput_capabilities{.gamepad = true,
            .low_frequency_rumble = true,
            .high_frequency_rumble = false});
    fixture.start();

    const auto resolved = fixture.service().resolve(fixture.selector());
    REQUIRE(resolved.has_value());
    CHECK((resolved->value.supported_channels & output::rumble_mask) == output::rumble_mask);
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::low_rumble));
    CHECK_FALSE(
        effects::has_feature(resolved->value.features, effects::feature_flags::high_rumble));
}

TEST_CASE("effects.capabilities_require_authoritative_usb_and_live_hd_audio",
    "[integration][effects][capabilities][sony][audio]") {
    auto metadata = std::make_unique<gameinput_test_support::fake_metadata_provider>();
    metadata->set_observation(available(metadata_device_model::dualsense),
        available(transport_metadata{.value = physical_transport::usb,
            .provenance = transport_provenance::native_bus_type}),
        false);
    metadata->set_capabilities(available(hardware_capabilities{.gamepad = true,
        .low_frequency_rumble = true,
        .high_frequency_rumble = true,
        .adaptive_left_trigger = true,
        .adaptive_right_trigger = true,
        .lightbar = true,
        .player_leds = true,
        .microphone_led = true}));
    session_fixture fixture{nullptr, {}, std::move(metadata)};
    configure_dualsense_facts(fixture);
    fixture.start();

    const auto resolved = fixture.service().resolve(fixture.selector());
    REQUIRE(resolved.has_value());
    CHECK((resolved->value.supported_channels & output::supported_channel_mask) ==
          output::supported_channel_mask);
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::left_trigger));
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::right_trigger));
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::lightbar));
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::player_leds));
    CHECK(effects::has_feature(resolved->value.features, effects::feature_flags::microphone_led));
    CHECK_FALSE(effects::has_feature(resolved->value.features, effects::feature_flags::hd_ready));
    CHECK(resolved->value.audio_instance_id == 0U);
}

TEST_CASE("effects.capabilities_reject_bluetooth_and_heuristic_usb_eligibility",
    "[integration][effects][capabilities][sony][eligibility]") {
    SECTION("Bluetooth") {
        auto metadata = std::make_unique<gameinput_test_support::fake_metadata_provider>();
        metadata->set_observation(available(metadata_device_model::dualsense),
            available(transport_metadata{.value = physical_transport::bluetooth,
                .provenance = transport_provenance::native_bus_type}),
            false);
        metadata->set_capabilities(available(hardware_capabilities{.gamepad = true,
            .low_frequency_rumble = true,
            .high_frequency_rumble = true,
            .adaptive_left_trigger = true,
            .adaptive_right_trigger = true}));
        session_fixture fixture{nullptr, {}, std::move(metadata)};
        configure_dualsense_facts(fixture);
        fixture.start_core_only();
        CHECK(waits_for_unsupported_target(fixture));
    }

    SECTION("Heuristic USB") {
        auto metadata = std::make_unique<gameinput_test_support::fake_metadata_provider>();
        metadata->set_observation(available(metadata_device_model::dualsense),
            available(transport_metadata{.value = physical_transport::usb,
                .provenance = transport_provenance::pnp_path_heuristic}),
            false);
        metadata->set_capabilities(available(hardware_capabilities{.gamepad = true,
            .low_frequency_rumble = true,
            .high_frequency_rumble = true,
            .adaptive_left_trigger = true,
            .adaptive_right_trigger = true}));
        session_fixture fixture{nullptr, {}, std::move(metadata)};
        configure_dualsense_facts(fixture);
        fixture.start_core_only();
        CHECK(waits_for_unsupported_target(fixture));
    }
}

TEST_CASE("effects.body_rejects_stale_audio_identity_before_native_commit",
    "[integration][effects][body][audio][identity]") {
    session_fixture fixture;
    fixture.start();
    const effects::body_request request{.context = fixture.open(),
        .audio_instance_id = 0xDEADBEEFU,
        .sequence = 1U,
        .ttl = 45s,
        .preference = output::body_route_preference::prefer_hd,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.4F},
        .pcm_command = output::body_pcm_command::keep};
    const auto submitted =
        fixture.service().submit_body(request, session_fixture::window(request.ttl));
    REQUIRE_FALSE(submitted.has_value());
    CHECK(submitted.error() == effects::status::stale_lease);
}

} // namespace inputnext::effects_test
