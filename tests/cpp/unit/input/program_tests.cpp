#include "input/program.hpp"

#include "values/required_value.hpp"
#include <catch2/catch_test_macros.hpp>

#include <inputnext/gamepad_state.hpp>

using namespace inputnext;
using namespace inputnext::input;
using inputnext::test::required_value;

TEST_CASE("input.identity_projector_preserves_all_existing_fields", "[unit][input]") {
    const gamepad_state state{
        .buttons = static_cast<button_mask>(gamepad_button::south) |
                   static_cast<button_mask>(gamepad_button::guide),
        .left_stick = {.x = -0.75F, .y = 0.5F},
        .right_stick = {.x = 0.25F, .y = -1.0F},
        .left_trigger = 0.4F,
        .right_trigger = 0.9F,
    };
    CHECK(project(physical_frame{.pad = state}, program::identity()) == state);
}

TEST_CASE("input.projector_applies_simultaneous_routes_without_cascading", "[unit][input]") {
    program_builder builder{program::identity()};
    REQUIRE(builder.set(source::face_bottom,
        route{.kind = target_kind::button, .button = gamepad_button::east}));
    REQUIRE(builder.set(source::face_right,
        route{.kind = target_kind::button, .button = gamepad_button::south}));
    const auto mappings = builder.build();
    REQUIRE(mappings.has_value());
    const auto compiled = required_value(mappings);

    physical_frame frame;
    frame.pad.buttons = static_cast<button_mask>(gamepad_button::south) |
                        static_cast<button_mask>(gamepad_button::guide);
    frame.pad.left_stick = {.x = 0.3F, .y = -0.4F};
    auto state = project(frame, compiled);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::east)) != 0U);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::south)) == 0U);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::guide)) != 0U);
    CHECK(state.left_stick == frame.pad.left_stick);

    frame.pad.buttons |= static_cast<button_mask>(gamepad_button::east);
    state = project(frame, compiled);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::south)) != 0U);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::east)) != 0U);
}

TEST_CASE("input.projector_merges_sources_and_projects_trigger_values", "[unit][input]") {
    program_builder builder{program::identity()};
    REQUIRE(builder.set(source::face_bottom,
        route{.kind = target_kind::button, .button = gamepad_button::view}));
    REQUIRE(builder.set(source::left_shoulder,
        route{.kind = target_kind::button, .button = gamepad_button::view}));
    REQUIRE(builder.set(source::left_trigger,
        route{.kind = target_kind::button, .button = gamepad_button::menu, .threshold = 0.7F}));
    REQUIRE(builder.set(source::right_trigger,
        route{.kind = target_kind::trigger, .right_trigger = false}));
    const auto mappings = builder.build();
    REQUIRE(mappings.has_value());
    const auto compiled = required_value(mappings);

    physical_frame frame;
    frame.pad.buttons = static_cast<button_mask>(gamepad_button::south);
    frame.pad.left_trigger = 0.69F;
    frame.pad.right_trigger = 0.85F;
    auto state = project(frame, compiled);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::menu)) == 0U);
    CHECK(state.left_trigger == 0.85F);

    frame.pad.buttons = static_cast<button_mask>(gamepad_button::left_shoulder);
    frame.pad.left_trigger = 0.7F;
    state = project(frame, compiled);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::menu)) != 0U);
}

TEST_CASE("input.projector_routes_buttons_to_binary_trigger_values", "[unit][input]") {
    program_builder builder{program::identity()};
    REQUIRE(builder.set(source::face_bottom,
        route{.kind = target_kind::trigger, .right_trigger = true}));
    REQUIRE(builder.set(source::right_trigger,
        route{.kind = target_kind::trigger, .right_trigger = false}));
    const auto mappings = builder.build();
    REQUIRE(mappings.has_value());
    const auto compiled = required_value(mappings);

    physical_frame frame;
    frame.pad.buttons = static_cast<button_mask>(gamepad_button::south);
    frame.pad.right_trigger = 0.25F;
    auto state = project(frame, compiled);
    CHECK(state.right_trigger == 1.0F);
    CHECK(state.left_trigger == 0.25F);

    frame.pad.buttons = 0U;
    state = project(frame, compiled);
    CHECK(state.right_trigger == 0.0F);
}

TEST_CASE("input.default_touch_routes_are_independent_physical_sources", "[unit][input]") {
    const auto mappings = program::defaults();
    REQUIRE(mappings.has_touch_routes());
    physical_frame frame;
    frame.pad.buttons = static_cast<button_mask>(gamepad_button::view);
    frame.touch = touch_source::left;
    const auto state = project(frame, mappings);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);

    frame.pad.buttons = 0U;
    frame.touch = touch_source::left;
    CHECK(
        (project(frame, mappings).buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
    frame.touch = touch_source::right;
    CHECK(
        (project(frame, mappings).buttons & static_cast<button_mask>(gamepad_button::menu)) != 0U);
    frame.touch = touch_source::fallback;
    CHECK(
        (project(frame, mappings).buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
    CHECK(program::identity().is_identity());
}

TEST_CASE("input.projector_rejects_guide_and_invalid_threshold_routes", "[unit][input]") {
    program_builder guide_builder{program::identity()};
    CHECK_FALSE(guide_builder.set(source::face_bottom,
        route{.kind = target_kind::button, .button = gamepad_button::guide}));
    CHECK_FALSE(guide_builder.build().has_value());

    program_builder threshold_builder{program::identity()};
    CHECK_FALSE(threshold_builder.set(source::face_bottom,
        route{.kind = target_kind::button, .button = gamepad_button::east, .threshold = 0.4F}));
    CHECK_FALSE(threshold_builder.build().has_value());
}
