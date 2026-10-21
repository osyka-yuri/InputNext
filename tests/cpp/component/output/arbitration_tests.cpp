#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <type_traits>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

TEST_CASE("output.arbitration_keeps_off_present_and_reveals_lower_owners", "[component][output]") {
    authority_fixture fixture{};
    const auto public_owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(public_owner.has_value());
    const internal_context_options diagnostic_options{
        .owner = internal_owner_key{201U},
        .priority = internal_priority::diagnostic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    const internal_context_options lower_options{
        .owner = internal_owner_key{202U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    const internal_context_options later_lower_options{
        .owner = internal_owner_key{203U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto diagnostic_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, diagnostic_options);
    auto lower_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, lower_options);
    auto later_lower_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, later_lower_options);
    const auto diagnostic = diagnostic_producer.open(fixture.connection.target);
    const auto lower = lower_producer.open(fixture.connection.target);
    const auto later_lower = later_lower_producer.open(fixture.connection.target);
    REQUIRE(diagnostic.has_value());
    REQUIRE(lower.has_value());
    REQUIRE(later_lower.has_value());

    const auto public_rumble = fixture.owner.submit(*public_owner,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.2F, .high = 0.3F}));
    REQUIRE(public_rumble.has_value());
    const auto diagnostic_rumble = diagnostic_producer.submit(*diagnostic,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.8F, .high = 0.9F}));
    REQUIRE(diagnostic_rumble.has_value());

    auto lower_trigger = inputnext::test::output::rumble_request(
        {.channels = left_trigger_mask, .sequence = 1U, .ttl = 5s});
    lower_trigger.values.left_trigger.mode = trigger_mode::weapon;
    lower_trigger.values.left_trigger.strength = 0.6F;
    REQUIRE(lower_producer.submit(*lower, lower_trigger).has_value());
    auto later_trigger = lower_trigger;
    later_trigger.values.left_trigger.strength = 0.9F;
    REQUIRE(later_lower_producer.submit(*later_lower, later_trigger).has_value());

    const auto off = inputnext::test::output::rumble_request(
        {.channels = left_trigger_mask, .sequence = 2U, .ttl = 5s});
    const auto public_off = fixture.owner.submit(*public_owner, off);
    REQUIRE(public_off.has_value());
    const auto selected = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(selected.has_value());
    CHECK(selected->present_channels() == (left_trigger_mask | rumble_mask));
    CHECK(selected->left_trigger.value_or(channel_snapshot<trigger_value>{}).value.mode ==
          trigger_mode::off);
    CHECK(selected->rumble.value_or(channel_snapshot<rumble_value>{}).value.low_frequency == 0.8F);
    CHECK(selected->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          diagnostic->identity);

    const clear_request clear_public_off{
        .selected_channels = left_trigger_mask,
        .sequence = 3U,
        .expected_revisions = {1U, 999U, 999U, 999U},
    };
    REQUIRE(fixture.owner.clear(*public_owner, clear_public_off).has_value());
    const auto revealed_trigger = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(revealed_trigger.has_value());
    CHECK(
        revealed_trigger->left_trigger.value_or(channel_snapshot<trigger_value>{}).value.strength ==
        0.9F);
    CHECK(revealed_trigger->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.identity == later_lower->identity);

    const clear_request clear_diagnostic{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = {999U, 999U, 1U, 999U},
    };
    REQUIRE(diagnostic_producer.clear(*diagnostic, clear_diagnostic).has_value());
    const auto revealed_rumble = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(revealed_rumble.has_value());
    CHECK(revealed_rumble->rumble.value_or(channel_snapshot<rumble_value>{}).value.low_frequency ==
          0.2F);
    CHECK(revealed_rumble->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          public_owner->identity);
}

TEST_CASE("output.identical_payload_with_new_sequence_advances_tie_order", "[component][output]") {
    authority_fixture fixture{};
    const auto public_owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(public_owner.has_value());

    const internal_context_options options{
        .owner = internal_owner_key{204U},
        .priority = internal_priority::game_semantic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto trusted = inputnext::test::output::make_internal_producer(fixture.trusted, options);
    const auto internal_owner = trusted.open(fixture.connection.target);
    REQUIRE(internal_owner.has_value());

    auto first_a = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.6F});
    first_a.selected_channels |= leds_mask;
    first_a.values.leds =
        led_value{.rgb = {12U, 34U, 56U}, .player_leds = 2U, .microphone_led = true};
    const auto accepted_a1 = fixture.owner.submit(*public_owner, first_a);
    REQUIRE(accepted_a1.has_value());

    const auto request_b1 = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.6F});
    const auto accepted_b1 = trusted.submit(*internal_owner, request_b1);
    REQUIRE(accepted_b1.has_value());

    const auto after_b1 = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_b1.has_value());
    REQUIRE(after_b1->rumble.has_value());
    REQUIRE(after_b1->leds.has_value());
    const auto rumble_after_b1 = after_b1->rumble.value_or(channel_snapshot<rumble_value>{});
    const auto leds_after_b1 = after_b1->leds.value_or(channel_snapshot<led_value>{});
    CHECK(rumble_after_b1.provenance.identity == internal_owner->identity);
    CHECK(rumble_after_b1.provenance.accepted_sequence == 1U);
    CHECK(rumble_after_b1.provenance.revision == 1U);
    CHECK(rumble_after_b1.value == request_b1.values.rumble);
    CHECK(leds_after_b1.provenance.identity == public_owner->identity);
    CHECK(leds_after_b1.provenance.accepted_sequence == 1U);
    CHECK(leds_after_b1.provenance.revision == 1U);
    CHECK(leds_after_b1.value == first_a.values.leds);

    auto request_a2 = first_a;
    request_a2.selected_channels = rumble_mask;
    request_a2.sequence = 2U;
    const auto accepted_a2 = fixture.owner.submit(*public_owner, request_a2);
    REQUIRE(accepted_a2.has_value());
    CHECK(accepted_a2->accepted_sequence == 2U);
    CHECK(accepted_a2->resulting_revisions[2U] == 2U);
    CHECK(accepted_a2->resulting_revisions[3U] == 1U);
    CHECK(accepted_a2->logical_version > accepted_b1->logical_version);

    const auto after_a2 = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_a2.has_value());
    REQUIRE(after_a2->rumble.has_value());
    const auto rumble_after_a2 = after_a2->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(rumble_after_a2.value == request_b1.values.rumble);
    CHECK(rumble_after_a2.provenance.identity == public_owner->identity);
    CHECK(rumble_after_a2.provenance.accepted_sequence == 2U);
    CHECK(rumble_after_a2.provenance.revision == 2U);
    CHECK(rumble_after_a2.provenance.accepted_commit_order == accepted_a2->logical_version);
    CHECK(after_a2->leds == after_b1->leds);

    const clear_request clear_a2{
        .selected_channels = rumble_mask,
        .sequence = 3U,
        .expected_revisions = accepted_a2->resulting_revisions,
    };
    REQUIRE(fixture.owner.clear(*public_owner, clear_a2).has_value());
    const auto revealed_b1 = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(revealed_b1.has_value());
    REQUIRE(revealed_b1->rumble.has_value());
    const auto rumble_revealed_b1 = revealed_b1->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(rumble_revealed_b1.value == request_b1.values.rumble);
    CHECK(rumble_revealed_b1.provenance.identity == internal_owner->identity);
    CHECK(rumble_revealed_b1.provenance.accepted_sequence == 1U);
    CHECK(rumble_revealed_b1.provenance.revision == 1U);
    CHECK(rumble_revealed_b1.provenance.accepted_commit_order == accepted_b1->logical_version);
    CHECK(revealed_b1->leds == after_b1->leds);
}

} // namespace
