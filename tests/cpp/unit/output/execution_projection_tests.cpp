#include "output/projection/represented.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::output::detail::native_values;
using inputnext::output::detail::project_representation;

[[nodiscard]] logical_snapshot make_snapshot() {
    logical_snapshot snapshot{};
    snapshot.target = {.runtime = {.bytes = {std::byte{0x51U}}},
        .device = device_key{7U},
        .connection = connection_id{11U}};
    snapshot.incarnation.bytes[0U] = std::byte{0x19U};
    snapshot.logical_version = 3U;
    snapshot.evaluated_at = std::chrono::steady_clock::time_point{120ms};
    snapshot.earliest_expiry = std::chrono::steady_clock::time_point{240ms};

    const context_identity identity{.target = snapshot.target,
        .incarnation = snapshot.incarnation,
        .id = context_id{5U},
        .kind = owner_class::trusted_internal,
        .internal_owner = internal_owner_key{31U}};
    snapshot.left_trigger = channel_snapshot<trigger_value>{
        .value = {.mode = trigger_mode::weapon, .start = 0.2F, .end = 0.7F, .strength = 0.6F},
        .provenance = {.identity = identity,
            .source_channel = channel::left_trigger,
            .revision = 4U,
            .accepted_sequence = 8U,
            .accepted_commit_order = 15U,
            .priority = internal_priority::game_profile}};
    snapshot.right_trigger = channel_snapshot<trigger_value>{
        .value = {.mode = trigger_mode::feedback, .start = 0.1F, .end = 0.9F, .strength = 0.3F},
        .provenance = {.identity = identity,
            .source_channel = channel::right_trigger,
            .revision = 3U,
            .accepted_sequence = 7U,
            .accepted_commit_order = 14U,
            .priority = internal_priority::game_profile}};
    snapshot.rumble =
        channel_snapshot<rumble_value>{.value = {.low_frequency = 0.4F, .high_frequency = 0.8F},
            .provenance = {.identity = identity,
                .source_channel = channel::rumble,
                .revision = 9U,
                .accepted_sequence = 12U,
                .accepted_commit_order = 21U,
                .priority = internal_priority::game_semantic}};
    snapshot.leds = channel_snapshot<led_value>{
        .value = {.rgb = {11U, 22U, 33U}, .player_leds = 3U, .microphone_led = true},
        .provenance = {.identity = identity,
            .source_channel = channel::leds,
            .revision = 2U,
            .accepted_sequence = 6U,
            .accepted_commit_order = 13U,
            .priority = internal_priority::game_profile}};
    return snapshot;
}

TEST_CASE("output.execution_projection_limits_values_and_provenance_to_supported_channels",
    "[unit][output]") {
    const auto snapshot = make_snapshot();
    const auto projected = project_representation(snapshot, rumble_mask | leds_mask);
    REQUIRE(projected.has_value());

    CHECK(projected->channels == (rumble_mask | leds_mask));
    CHECK_FALSE(projected->left_trigger.has_value());
    CHECK_FALSE(projected->right_trigger.has_value());
    REQUIRE(projected->rumble.has_value());
    const auto projected_rumble = projected->rumble.value_or(channel_snapshot<rumble_value>{});
    const auto source_rumble = snapshot.rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(projected_rumble.value == source_rumble.value);
    CHECK(projected_rumble.provenance == source_rumble.provenance);
    REQUIRE(projected->leds.has_value());
    const auto projected_leds = projected->leds.value_or(channel_snapshot<led_value>{});
    const auto source_leds = snapshot.leds.value_or(channel_snapshot<led_value>{});
    CHECK(projected_leds.provenance == source_leds.provenance);

    const auto output = native_values(*projected);
    CHECK(output.channels == (rumble_mask | leds_mask));
    CHECK(output.values.rumble == source_rumble.value);
    CHECK(output.values.leds == source_leds.value);
    CHECK(output.values.left_trigger == trigger_value{});
    CHECK(output.values.right_trigger == trigger_value{});
}

TEST_CASE("output.execution_projection_distinguishes_present_neutral_intents_from_absence",
    "[unit][output]") {
    auto present = make_snapshot();
    const auto present_rumble = present.rumble.value_or(channel_snapshot<rumble_value>{});
    present.rumble =
        channel_snapshot<rumble_value>{.value = {}, .provenance = present_rumble.provenance};
    const auto present_projection = project_representation(present, rumble_mask);
    REQUIRE(present_projection.has_value());

    auto absent = present;
    absent.rumble.reset();
    const auto absent_projection = project_representation(absent, rumble_mask);
    REQUIRE(absent_projection.has_value());

    CHECK(present_projection->rumble.has_value());
    CHECK_FALSE(absent_projection->rumble.has_value());
    CHECK(native_values(*present_projection) == native_values(*absent_projection));
    CHECK(*present_projection != *absent_projection);
}

TEST_CASE("output.execution_projection_ignores_unrepresented_channels_and_global_versions",
    "[unit][output]") {
    const auto original = make_snapshot();
    const auto expected = project_representation(original, rumble_mask);
    REQUIRE(expected.has_value());

    auto unrelated_change = original;
    unrelated_change.logical_version += 100U;
    unrelated_change.evaluated_at += 5s;
    unrelated_change.earliest_expiry = std::chrono::steady_clock::time_point{9s};
    auto unrelated_trigger =
        unrelated_change.left_trigger.value_or(channel_snapshot<trigger_value>{});
    unrelated_trigger.value.strength = 0.1F;
    unrelated_trigger.provenance.revision += 1U;
    unrelated_change.left_trigger = unrelated_trigger;
    const auto current = project_representation(unrelated_change, rumble_mask);
    REQUIRE(current.has_value());
    CHECK(*current == *expected);

    auto changed_rumble = unrelated_change.rumble.value_or(channel_snapshot<rumble_value>{});
    changed_rumble.provenance.accepted_sequence += 1U;
    unrelated_change.rumble = changed_rumble;
    const auto changed_owner = project_representation(unrelated_change, rumble_mask);
    REQUIRE(changed_owner.has_value());
    CHECK(*changed_owner != *expected);
}

TEST_CASE("output.execution_projection_rejects_invalid_targets_and_channel_masks",
    "[unit][output]") {
    auto snapshot = make_snapshot();
    const auto empty = project_representation(snapshot, 0U);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == execution_error::invalid_argument);

    const auto unknown_bits = project_representation(snapshot, supported_channel_mask << 1U);
    REQUIRE_FALSE(unknown_bits.has_value());
    CHECK(unknown_bits.error() == execution_error::invalid_argument);

    snapshot.target.connection = {};
    const auto invalid_target = project_representation(snapshot, rumble_mask);
    REQUIRE_FALSE(invalid_target.has_value());
    CHECK(invalid_target.error() == execution_error::invalid_argument);
}

} // namespace
