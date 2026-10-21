#include <inputnext/output.hpp>

#include "output/policy/policy.hpp"
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace inputnext::output;
using namespace inputnext::output::policy_detail;

namespace {

[[nodiscard]] submit_request valid_request() {
    return submit_request{
        .selected_channels = rumble_mask,
        .sequence = 1U,
        .ttl = std::chrono::milliseconds{1},
        .values = {},
    };
}

[[nodiscard]] candidate_view candidate(const channel source,
    const internal_priority priority,
    const std::uint64_t order,
    const optional_deadline expiry = {}) {
    return candidate_view{
        .source_channel = source,
        .priority = priority,
        .accepted_commit_order = order,
        .expires_at = expiry,
    };
}

} // namespace

TEST_CASE("output.policy.validates_all_values_even_when_unselected", "[unit][output]") {
    auto request = valid_request();
    request.selected_channels = rumble_mask;
    request.values.left_trigger = trigger_value{
        .mode = trigger_mode::off,
        .layout = trigger_layout::uniform,
        .start = 0.0F,
        .end = 1.0F,
        .strength = 1.0F,
        .frequency = 255.0F,
        .zones = {8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U},
    };
    request.values.right_trigger.mode = trigger_mode::vibration;
    request.values.right_trigger.layout = trigger_layout::per_zone;
    request.values.right_trigger.zones.fill(maximum_trigger_zone_level);
    request.values.rumble = {.low_frequency = 1.0F, .high_frequency = 1.0F};
    request.values.leds = {.rgb = {0U, 127U, 255U}, .player_leds = 4U, .microphone_led = true};
    REQUIRE(validate_submit_request(request));

    SECTION("non-finite trigger fields are rejected") {
        request.values.left_trigger.start = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(validate_submit_request(request));

        request.values.left_trigger.start = 0.0F;
        request.values.right_trigger.end = std::numeric_limits<float>::infinity();
        CHECK_FALSE(validate_submit_request(request));

        request.values.right_trigger.end = 1.0F;
        request.values.left_trigger.strength = -std::numeric_limits<float>::infinity();
        CHECK_FALSE(validate_submit_request(request));
    }

    SECTION("trigger ranges and enum values are strict") {
        request.values.left_trigger.start = -0.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.start = 0.0F;

        request.values.left_trigger.end = 1.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.end = 0.5F;
        request.values.left_trigger.start = 0.6F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.start = 0.0F;

        request.values.left_trigger.strength = -0.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.strength = 1.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.strength = 1.0F;

        request.values.left_trigger.frequency = 12.5F;
        REQUIRE(validate_submit_request(request));
        CHECK(request.values.left_trigger.frequency == 12.5F);
        request.values.left_trigger.frequency = 256.0F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.frequency = 255.0F;

        request.values.left_trigger.frequency = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.frequency = 255.0F;
        request.values.right_trigger.frequency = std::numeric_limits<float>::infinity();
        CHECK_FALSE(validate_submit_request(request));
        request.values.right_trigger.frequency = 255.0F;
        request.values.left_trigger.frequency = -0.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.frequency = 255.0F;
        request.values.left_trigger.mode = std::bit_cast<trigger_mode>(std::uint8_t{255U});
        CHECK_FALSE(validate_submit_request(request));
        request.values.left_trigger.mode = trigger_mode::off;
        request.values.right_trigger.layout = std::bit_cast<trigger_layout>(std::uint8_t{255U});
        CHECK_FALSE(validate_submit_request(request));
    }

    SECTION("all ten zone values are checked for either layout") {
        request.values.left_trigger.zones[9U] = 9U;
        CHECK_FALSE(validate_submit_request(request));
    }

    SECTION("rumble and LED limits are checked") {
        request.values.rumble.low_frequency = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(validate_submit_request(request));
        request.values.rumble.low_frequency = 1.0F;
        request.values.rumble.high_frequency = std::numeric_limits<float>::infinity();
        CHECK_FALSE(validate_submit_request(request));
        request.values.rumble.high_frequency = -0.01F;
        CHECK_FALSE(validate_submit_request(request));
        request.values.rumble.high_frequency = 1.0F;

        request.values.leds.player_leds = 5U;
        CHECK_FALSE(validate_submit_request(request));
    }
}

TEST_CASE("output.policy.validates_masks_sequences_and_ttl_permissions", "[unit][output]") {
    auto request = valid_request();
    CHECK(validate_submit_request(request));

    request.selected_channels = 0U;
    CHECK_FALSE(validate_submit_request(request));
    request.selected_channels = supported_channel_mask | (1U << 12U);
    CHECK_FALSE(validate_submit_request(request));
    request.selected_channels = rumble_mask;

    request.sequence = 0U;
    CHECK_FALSE(validate_submit_request(request));
    request.sequence = 1U;

    request.ttl = std::chrono::milliseconds{0};
    CHECK_FALSE(validate_submit_request(request));
    CHECK_FALSE(validate_internal_submit_request(request, ttl_permission::finite_only));
    CHECK(validate_internal_submit_request(request, ttl_permission::finite_or_indefinite));

    request.ttl = std::chrono::milliseconds{1};
    CHECK(validate_submit_request(request));
    request.ttl = maximum_finite_ttl;
    CHECK(validate_submit_request(request));
    request.ttl = maximum_finite_ttl + std::chrono::milliseconds{1};
    CHECK_FALSE(validate_submit_request(request));
    CHECK_FALSE(validate_internal_submit_request(request, ttl_permission::finite_or_indefinite));

    request.ttl = std::chrono::milliseconds{0};
    CHECK_FALSE(validate_internal_submit_request(request,
        std::bit_cast<ttl_permission>(std::uint8_t{255U})));

    const internal_context_options valid_options{
        .owner = internal_owner_key{1U},
        .priority = internal_priority::diagnostic,
        .ttl = ttl_permission::finite_or_indefinite,
        .foreground = foreground_policy::foreground_bound,
    };
    CHECK(validate_internal_context_options(valid_options));
    auto invalid_options = valid_options;
    invalid_options.owner = {};
    CHECK_FALSE(validate_internal_context_options(invalid_options));
    invalid_options = valid_options;
    invalid_options.priority = std::bit_cast<internal_priority>(std::uint8_t{30U});
    CHECK_FALSE(validate_internal_context_options(invalid_options));
    invalid_options = valid_options;
    invalid_options.foreground = std::bit_cast<foreground_policy>(std::uint8_t{255U});
    CHECK_FALSE(validate_internal_context_options(invalid_options));
}

TEST_CASE("output.policy.clear_ignores_unselected_revision_fields", "[unit][output]") {
    const clear_request request{
        .selected_channels = left_trigger_mask,
        .sequence = 1U,
        .expected_revisions = {0U,
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max()},
    };
    CHECK(validate_clear_request(request));

    auto invalid = request;
    invalid.selected_channels = 0U;
    CHECK_FALSE(validate_clear_request(invalid));
    invalid = request;
    invalid.sequence = 0U;
    CHECK_FALSE(validate_clear_request(invalid));
}

TEST_CASE("output.policy.deadlines_are_checked_and_indefinite_is_absent", "[unit][output]") {
    const auto entry = deadline{clock::duration{100}};
    const auto finite =
        make_deadline(entry, std::chrono::milliseconds{1}, ttl_permission::finite_only);
    REQUIRE(finite);
    REQUIRE(finite->has_value());
    CHECK(finite->value_or(deadline{}) == entry + std::chrono::milliseconds{1});

    const auto near_maximum = deadline::max() - std::chrono::milliseconds{10};
    const auto reaches_maximum =
        make_deadline(near_maximum, std::chrono::milliseconds{10}, ttl_permission::finite_only);
    REQUIRE(reaches_maximum);
    REQUIRE(reaches_maximum->has_value());
    CHECK(reaches_maximum->value_or(deadline{}) == deadline::max());

    const auto overflow =
        make_deadline(near_maximum, std::chrono::milliseconds{11}, ttl_permission::finite_only);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == error::deadline_overflow);

    const auto indefinite = make_deadline(deadline::max(),
        std::chrono::milliseconds{0},
        ttl_permission::finite_or_indefinite);
    REQUIRE(indefinite);
    CHECK_FALSE(indefinite->has_value());

    const auto unauthorized =
        make_deadline(entry, std::chrono::milliseconds{0}, ttl_permission::finite_only);
    CHECK_FALSE(unauthorized);
    const auto negative =
        make_deadline(entry, std::chrono::milliseconds{-1}, ttl_permission::finite_or_indefinite);
    CHECK_FALSE(negative);
}

TEST_CASE("output.policy.selects_independent_whole_channel_winners", "[unit][output]") {
    const auto now = deadline{clock::duration{1000}};
    const auto expires_now = now;
    const auto expires_later = now + std::chrono::milliseconds{1};
    const std::array candidates{
        candidate(channel::left_trigger, internal_priority::game_semantic, 1U),
        candidate(channel::left_trigger, internal_priority::game_semantic, 2U),
        candidate(channel::left_trigger, internal_priority::diagnostic, 3U, expires_now),
        candidate(channel::rumble, internal_priority::generic_profile, 4U),
        candidate(channel::rumble, internal_priority::game_profile, 5U),
        candidate(channel::leds, internal_priority::game_profile, 6U, expires_later),
        candidate(channel::leds, internal_priority::lowest, 7U),
    };

    std::array<trigger_value, 7U> trigger_payloads{};
    trigger_payloads[0U].mode = trigger_mode::feedback;
    trigger_payloads[1U].mode = trigger_mode::off;
    std::array<rumble_value, 7U> rumble_payloads{};
    rumble_payloads[3U] = {.low_frequency = 0.8F, .high_frequency = 0.0F};
    rumble_payloads[4U] = {.low_frequency = 0.0F, .high_frequency = 0.75F};

    const auto winners = select_winners(candidates, now);
    REQUIRE(winners);
    CHECK((*winners)[0U] == 1U);
    CHECK(trigger_payloads[(*winners)[0U].value_or(0U)].mode == trigger_mode::off);
    CHECK((*winners)[1U] == std::nullopt);
    CHECK((*winners)[2U] == 4U);
    const rumble_value expected_whole_rumble{.low_frequency = 0.0F, .high_frequency = 0.75F};
    CHECK(rumble_payloads[(*winners)[2U].value_or(0U)] == expected_whole_rumble);
    CHECK((*winners)[3U] == 5U);

    const auto at_expiry = select_winners(candidates, expires_later);
    REQUIRE(at_expiry);
    CHECK((*at_expiry)[3U] == 6U);
}

TEST_CASE("output.policy.rejects_unbounded_or_malformed_candidates", "[unit][output]") {
    const auto now = deadline{clock::duration{1000}};
    const std::array malformed{candidate(channel::rumble, internal_priority::game_profile, 0U)};
    const auto invalid_order = select_winners(malformed, now);
    REQUIRE_FALSE(invalid_order);
    CHECK(invalid_order.error() == error::invalid_argument);

    const std::array invalid_priority{
        candidate(channel::rumble, std::bit_cast<internal_priority>(std::uint8_t{30U}), 1U),
    };
    CHECK_FALSE(select_winners(invalid_priority, now));

    const std::array invalid_channel{
        candidate(std::bit_cast<channel>(std::uint8_t{255U}), internal_priority::game_profile, 1U),
    };
    CHECK_FALSE(select_winners(invalid_channel, now));

    const std::vector<candidate_view> oversized(maximum_candidate_count + 1U,
        candidate(channel::rumble, internal_priority::game_profile, 1U));
    const auto over_capacity = select_winners(oversized, now);
    REQUIRE_FALSE(over_capacity);
    CHECK(over_capacity.error() == error::capacity_exceeded);
}
