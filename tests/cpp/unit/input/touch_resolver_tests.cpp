#include "input/program.hpp"
#include "input/touch_resolver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace inputnext::input;

namespace {

[[nodiscard]] touch_observation released(std::uint64_t timestamp = 1U) {
    touch_observation value;
    value.click_status = observation_status::observed;
    value.click_pressed = false;
    value.source_timestamp = timestamp;
    return value;
}

[[nodiscard]] touch_observation pressed(const double coordinate_x, std::uint64_t timestamp = 2U) {
    touch_observation value;
    value.click_status = observation_status::observed;
    value.click_pressed = true;
    value.contacts_status = observation_status::observed;
    value.same_reading = true;
    value.contacts_complete = true;
    value.surface_width = 100.0;
    value.surface_height = 50.0;
    value.contacts[0] = touch_contact{.id = 4U, .x = coordinate_x, .y = 20.0, .active = true};
    value.contact_count = 1U;
    value.source_timestamp = timestamp;
    return value;
}

} // namespace

TEST_CASE("input.touch_resolver_requires_release_before_resolving_initial_held_click",
    "[unit][input]") {
    touch_resolver resolver;
    auto state = resolver.observe(pressed(10.0F));
    CHECK(state.source == touch_source::fallback);
    CHECK(state.click_known_pressed);
    CHECK(state.first_accepted_timestamp == 2U);

    state = resolver.observe(released());
    CHECK(state.source == touch_source::none);
    CHECK_FALSE(state.click_known_pressed);
    CHECK(state.first_accepted_timestamp == 2U);
    state = resolver.observe(pressed(10.0F, 3U));
    CHECK(state.source == touch_source::left);
    CHECK(state.first_accepted_timestamp == 2U);
}

TEST_CASE("input.touch_resolver_chooses_minimum_contact_and_right_includes_midline",
    "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released()));
    auto observation = pressed(90.0F);
    observation.contact_count = 2U;
    observation.contacts[0] = touch_contact{.id = 12U, .x = 10.0, .y = 4.0, .active = true};
    observation.contacts[1] = touch_contact{.id = 2U, .x = 50.0, .y = 4.0, .active = true};
    auto state = resolver.observe(observation);
    CHECK(state.source == touch_source::right);

    observation = released(4U);
    static_cast<void>(resolver.observe(observation));
    state = resolver.observe(pressed(49.99F, 5U));
    CHECK(state.source == touch_source::left);
}

TEST_CASE("input.touch_resolver_preserves_uint32_coordinate_precision", "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released()));
    auto observation = pressed(8'388'608.0);
    observation.surface_width = 16'777'217.0;
    const auto state = resolver.observe(observation);
    CHECK(state.source == touch_source::left);
}

TEST_CASE("input.touch_resolver_latches_side_until_release", "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released()));
    auto state = resolver.observe(pressed(10.0F));
    REQUIRE(state.source == touch_source::left);
    auto movement = pressed(90.0F, 3U);
    movement.contacts_status = observation_status::unavailable;
    movement.same_reading = false;
    movement.contacts_complete = false;
    state = resolver.observe(movement);
    CHECK(state.source == touch_source::left);
    CHECK(state.first_accepted_timestamp == 1U);

    static_cast<void>(resolver.observe(released(4U)));
    state = resolver.observe(pressed(90.0F, 5U));
    CHECK(state.source == touch_source::right);
}

TEST_CASE("input.touch_resolver_invalidates_lost_click_and_waits_for_release", "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released(10U)));
    auto state = resolver.observe(pressed(15.0F, 20U));
    REQUIRE(state.source == touch_source::left);
    REQUIRE(state.first_accepted_timestamp == 10U);
    const auto previous_epoch = state.epoch;

    touch_observation lost;
    lost.click_status = observation_status::unavailable;
    state = resolver.observe(lost);
    CHECK(state.source == touch_source::none);
    CHECK(state.awaiting_release);
    CHECK(state.epoch == previous_epoch + 1U);
    CHECK(state.reason == touch_loss_reason::missing_click);
    CHECK_FALSE(state.first_accepted_timestamp.has_value());

    state = resolver.observe(pressed(85.0F, 100U));
    CHECK(state.source == touch_source::none);
    CHECK(state.awaiting_release);
    CHECK(state.epoch == previous_epoch + 1U);
    CHECK(state.first_accepted_timestamp == 100U);

    state = resolver.observe(lost);
    CHECK(state.epoch == previous_epoch + 1U);
    CHECK(state.first_accepted_timestamp == 100U);

    static_cast<void>(resolver.observe(released(200U)));
    CHECK(resolver.snapshot().first_accepted_timestamp == 100U);
    state = resolver.observe(pressed(85.0F, 300U));
    CHECK(state.source == touch_source::right);
    CHECK_FALSE(state.awaiting_release);
    CHECK(state.first_accepted_timestamp == 100U);
}

TEST_CASE("input.touch_resolver_rejects_stale_or_malformed_coordinates", "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released()));
    auto observation = pressed(10.0F);
    observation.same_reading = false;
    auto state = resolver.observe(observation);
    CHECK(state.source == touch_source::fallback);

    static_cast<void>(resolver.observe(released(3U)));
    observation = pressed(10.0F, 4U);
    observation.contact_count = 2U;
    observation.contacts[1] = observation.contacts[0];
    state = resolver.observe(observation);
    CHECK(state.source == touch_source::fallback);

    static_cast<void>(resolver.observe(released(5U)));
    observation = pressed(101.0F, 6U);
    state = resolver.observe(observation);
    CHECK(state.source == touch_source::fallback);

    static_cast<void>(resolver.observe(released(7U)));
    observation = pressed(100.0F, 8U);
    state = resolver.observe(observation);
    CHECK(state.source == touch_source::fallback);
}

TEST_CASE("input.touch_resolver_non_applicable_and_unchanged_do_not_mutate", "[unit][input]") {
    touch_resolver resolver;
    static_cast<void>(resolver.observe(released()));
    const auto before = resolver.observe(pressed(10.0F));
    auto unrelated = pressed(90.0F, 99U);
    unrelated.click_status = observation_status::not_applicable;
    auto after = resolver.observe(unrelated);
    CHECK(after == before);
    unrelated.click_status = observation_status::unchanged;
    after = resolver.observe(unrelated);
    CHECK(after == before);
}
