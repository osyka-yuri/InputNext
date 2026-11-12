#include "input/program.hpp"
#include "profiles/parser.hpp"

#include "values/required_value.hpp"
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <inputnext/gamepad_state.hpp>
#include <inputnext/output.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using namespace inputnext;
using namespace inputnext::input;
using namespace inputnext::profiles;
using inputnext::test::required_value;

namespace {

[[nodiscard]] gamepad_state project(const parsed_profile& parsed,
    const button_mask buttons,
    const touch_source touch = touch_source::none,
    const float left_trigger = 0.0F) {
    physical_frame frame;
    frame.pad.buttons = buttons;
    frame.pad.left_trigger = left_trigger;
    frame.touch = touch;
    return inputnext::input::project(frame, parsed.settings.input);
}

[[nodiscard]] bool has_diagnostic(const parsed_profile& parsed, const std::string_view text) {
    return std::ranges::any_of(parsed.diagnostics,
        [text](const diagnostic& item) { return item.message.contains(text); });
}

} // namespace

TEST_CASE("profiles.input_defaults_keep_standard_routes_and_touch_mappings", "[unit][profiles]") {
    auto parsed = parse_profile("");
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->settings.input.is_identity());
    CHECK(parsed->settings.input.has_touch_routes());

    auto state = project(*parsed, static_cast<button_mask>(gamepad_button::south));
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::south)) != 0U);
    state = project(*parsed, 0U, touch_source::left);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
    state = project(*parsed, 0U, touch_source::right);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::menu)) != 0U);
    state = project(*parsed, 0U, touch_source::fallback);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::view)) != 0U);
}

TEST_CASE("profiles.input_maps_sources_simultaneously_and_keeps_touch_independent",
    "[unit][profiles]") {
    std::string_view document;
    SECTION("Xbox names") {
        document = R"(
[input]
remap = [
    { from = "a", to = "b" },
    { from = "b", to = "a" },
    { from = "view", to = "disabled" },
    { from = "lb", to = "lt" },
    { from = "lt", to = "lb", threshold = 0.6 },
    { from = "rb", to = "rt" },
    { from = "rt", to = "rb" },
    { from = "touchpad_left", to = "view" },
    { from = "touchpad_right", to = "y" },
    { from = "touchpad_unknown_side", to = "x" },
]
layout = "xbox"
)";
    }
    SECTION("PlayStation names with a universal destination") {
        document = R"(
[input]
layout = "playstation"
remap = [
    { from = "cross", to = "circle" },
    { from = "circle", to = "cross" },
    { from = "create", to = "disabled" },
    { from = "l1", to = "l2" },
    { from = "l2", to = "l1", threshold = 0.6 },
    { from = "r1", to = "r2" },
    { from = "r2", to = "r1" },
    { from = "touchpad_left", to = "create" },
    { from = "touchpad_right", to = "face_top" },
    { from = "touchpad_unknown_side", to = "square" },
]
)";
    }
    const auto parsed = parse_profile(document);
    REQUIRE(parsed.has_value());
    const auto& profile = parsed.value();
    REQUIRE(profile.diagnostics.empty());

    CHECK(project(profile, static_cast<button_mask>(gamepad_button::south)).buttons ==
          static_cast<button_mask>(gamepad_button::east));
    CHECK(project(profile, static_cast<button_mask>(gamepad_button::east)).buttons ==
          static_cast<button_mask>(gamepad_button::south));
    CHECK(project(profile, static_cast<button_mask>(gamepad_button::west)).buttons ==
          static_cast<button_mask>(gamepad_button::west));
    CHECK(project(profile, static_cast<button_mask>(gamepad_button::view)).buttons == 0U);
    CHECK(project(profile, 0U, touch_source::left).buttons ==
          static_cast<button_mask>(gamepad_button::view));
    CHECK(project(profile, 0U, touch_source::right).buttons ==
          static_cast<button_mask>(gamepad_button::north));
    CHECK(project(profile, 0U, touch_source::fallback).buttons ==
          static_cast<button_mask>(gamepad_button::west));
    CHECK(project(profile, 0U, touch_source::none, 0.59F).buttons == 0U);
    CHECK(project(profile, 0U, touch_source::none, 0.6F).buttons ==
          static_cast<button_mask>(gamepad_button::left_shoulder));
    CHECK(project(profile, static_cast<button_mask>(gamepad_button::left_shoulder)).left_trigger ==
          1.0F);
    CHECK(
        project(profile, static_cast<button_mask>(gamepad_button::right_shoulder)).right_trigger ==
        1.0F);
    const physical_frame right_trigger_press{.pad = {.right_trigger = 0.5F}};
    CHECK(inputnext::input::project(right_trigger_press, profile.settings.input).buttons ==
          static_cast<button_mask>(gamepad_button::right_shoulder));
}

TEST_CASE("profiles.input_layout_names_compile_to_the_same_button_routes", "[unit][profiles]") {
    static constexpr std::array buttons{gamepad_button::dpad_up,
        gamepad_button::dpad_down,
        gamepad_button::dpad_left,
        gamepad_button::dpad_right,
        gamepad_button::menu,
        gamepad_button::view,
        gamepad_button::left_stick,
        gamepad_button::right_stick,
        gamepad_button::left_shoulder,
        gamepad_button::right_shoulder,
        gamepad_button::south,
        gamepad_button::east,
        gamepad_button::west,
        gamepad_button::north};

    struct layout_case final {
        std::string_view name;
        std::array<std::string_view, buttons.size()> controls;
    };

    constexpr std::array layouts{
        layout_case{.name = "universal",
            .controls = {"dpad_up",
                "dpad_down",
                "dpad_left",
                "dpad_right",
                "menu",
                "select",
                "left_stick_click",
                "right_stick_click",
                "left_shoulder",
                "right_shoulder",
                "face_bottom",
                "face_right",
                "face_left",
                "face_top"}},
        layout_case{.name = "playstation",
            .controls = {"dpad_up",
                "dpad_down",
                "dpad_left",
                "dpad_right",
                "options",
                "create",
                "l3",
                "r3",
                "l1",
                "r1",
                "cross",
                "circle",
                "square",
                "triangle"}},
        layout_case{.name = "xbox",
            .controls = {"dpad_up",
                "dpad_down",
                "dpad_left",
                "dpad_right",
                "menu",
                "view",
                "ls",
                "rs",
                "lb",
                "rb",
                "a",
                "b",
                "x",
                "y"}},
    };
    std::optional<program> canonical;
    for (const auto& layout : layouts) {
        CAPTURE(layout.name);
        std::string document{"[input]\nlayout = '"};
        document.append(layout.name).append("'\nremap = [\n");
        for (std::size_t index = 0U; index < buttons.size(); ++index) {
            document.append("{ from = '")
                .append(layout.controls[index])
                .append("', to = '")
                .append(layout.controls[(index + 1U) % buttons.size()])
                .append("' },\n");
        }
        document.append("]\n");
        const auto parsed = parse_profile(document);
        REQUIRE(parsed.has_value());
        const auto& profile = parsed.value();
        REQUIRE(profile.diagnostics.empty());
        if (!canonical.has_value()) {
            canonical = profile.settings.input;
        }
        CHECK(profile.settings.input == canonical.value());
        for (std::size_t index = 0U; index < buttons.size(); ++index) {
            CAPTURE(layout.controls[index]);
            const auto expected = buttons[(index + 1U) % buttons.size()];
            CHECK(project(profile, static_cast<button_mask>(buttons[index])).buttons ==
                  static_cast<button_mask>(expected));
        }
    }
}

TEST_CASE("profiles.input_rejects_invalid_explicit_rules_atomically", "[unit][profiles]") {
    constexpr auto invalid_rules = std::to_array<std::string_view>({
        "'circle'",
        "{ to = 'circle' }",
        "{ from = 'square' }",
        "{ from = 1, to = 'circle' }",
        "{ from = 'square', to = true }",
        "{ from = 'disabled', to = 'circle' }",
        "{ from = 'square', to = 'touchpad_left' }",
        "{ from = 'square', to = 'circle', extra = true }",
        "{ from = 'square', to = 'circle', threshold = 0.5 }",
        "{ from = 'l2', to = 'r2', threshold = 0.5 }",
        "{ from = 'l2', to = 'disabled', threshold = 0.5 }",
        "{ from = 'l2', to = 'l1', threshold = '0.5' }",
        "{ from = 'l2', to = 'l1', threshold = nan }",
        "{ from = 'l2', to = 'l1', threshold = 0 }",
        "{ from = 'l2', to = 'l1', threshold = 1.1 }",
    });
    for (const auto invalid_rule : invalid_rules) {
        CAPTURE(invalid_rule);
        std::string document{"[input]\nlayout = 'playstation'\n"
                             "remap = [{ from = 'cross', to = 'circle' }, "};
        document.append(invalid_rule);
        document.append("]\n[effects.left_trigger]\nmode = 'feedback'\nstrength = 0.75\n");
        const auto parsed = parse_profile(document);
        REQUIRE(parsed.has_value());
        const auto& profile = parsed.value();
        REQUIRE_FALSE(profile.diagnostics.empty());
        CHECK(profile.diagnostics.front().line == 3U);
        CHECK(profile.settings.input == program::defaults());
        CHECK(required_value(profile.settings.effects.left_trigger).mode ==
              output::trigger_mode::feedback);
    }
}

TEST_CASE("profiles.input_rejects_duplicate_physical_sources_after_name_resolution",
    "[unit][profiles]") {
    constexpr std::array<std::string_view, 4U> duplicates{
        "[input]\nlayout = 'playstation'\n"
        "remap = [{ from = 'cross', to = 'circle' }, { from = 'cross', to = 'square' }]",
        "[input]\nlayout = 'playstation'\n"
        "remap = [{ from = 'cross', to = 'circle' }, { from = 'face_bottom', to = 'square' }]",
        "[input]\nlayout = 'xbox'\n"
        "remap = [{ from = 'lt', to = 'lb' }, { from = 'left_trigger', to = 'rb' }]",
        "[input]\nremap = [{ from = 'touchpad_left', to = 'face_top' }, "
        "{ from = 'touchpad_left', to = 'select' }]",
    };
    for (const auto document : duplicates) {
        CAPTURE(document);
        const auto parsed = parse_profile(document);
        REQUIRE(parsed.has_value());
        const auto& profile = parsed.value();
        CHECK(has_diagnostic(profile, "Duplicate physical input source"));
        CHECK(profile.settings.input == program::defaults());
    }
}

TEST_CASE("profiles.input_accepts_an_empty_rule_list_and_defaults_to_universal_names",
    "[unit][profiles]") {
    const auto defaults = parse_profile("[input]\nremap = []");
    REQUIRE(defaults.has_value());
    CHECK(defaults.value().diagnostics.empty());
    CHECK(defaults.value().settings.input == program::defaults());

    const auto parsed = parse_profile(R"(
[input]
remap = [{ from = "touchpad_left", to = "face_top" },
    { from = "face_bottom", to = "face_right" }]
)");
    REQUIRE(parsed.has_value());
    const auto& profile = parsed.value();
    REQUIRE(profile.diagnostics.empty());
    CHECK(project(profile, 0U, touch_source::left).buttons ==
          static_cast<button_mask>(gamepad_button::north));
    CHECK(project(profile, 0U, touch_source::right).buttons ==
          static_cast<button_mask>(gamepad_button::menu));
    CHECK(project(profile, static_cast<button_mask>(gamepad_button::south)).buttons ==
          static_cast<button_mask>(gamepad_button::east));
}

TEST_CASE("profiles.any_input_semantic_error_discards_all_input_but_keeps_effects",
    "[unit][profiles]") {
    const auto parsed = parse_profile(R"(
[input]
remap = [
    { from = "face_bottom", to = "face_right" },
    { from = "unknown_source", to = "face_top" },
]

[effects.left_trigger]
mode = "feedback"
strength = 0.75
)");
    REQUIRE(parsed.has_value());
    REQUIRE_FALSE(parsed->diagnostics.empty());
    CHECK(has_diagnostic(*parsed, "Unknown input remap source"));
    CHECK(parsed->settings.input == program::defaults());
    REQUIRE(parsed->settings.effects.left_trigger.has_value());
    CHECK(required_value(parsed->settings.effects.left_trigger).mode ==
          output::trigger_mode::feedback);
}

TEST_CASE("profiles.input_enforces_the_selected_layout_and_one_rule_array", "[unit][profiles]") {
    constexpr auto invalid_inputs = std::to_array<std::string_view>({
        "input = 'bad'",
        "[input]\nlayout = 5\nremap = []",
        "[input]\nlayout = 'other'\nremap = []",
        "[input]\nremap = [{ from = 'cross', to = 'face_right' }]",
        "[input]\nlayout = 'xbox'\nremap = [{ from = 'a', to = 'circle' }]",
        "[input]\nlayout = 'playstation'\nremap = [{ from = 'a', to = 'circle' }]",
        "[input]\nremap = 'bad'",
        "[input.remap]\nsouth = 'east'",
        "[input.touchpad]\nleft = 'view'",
        "[input]\nremap = [{ from = 'face_bottom', to = 'face_right' }]\n[input.unknown]",
    });
    for (const auto invalid_input : invalid_inputs) {
        CAPTURE(invalid_input);
        std::string document{invalid_input};
        document.append("\n[effects.left_trigger]\nmode = 'feedback'\nstrength = 0.75\n");
        const auto parsed = parse_profile(document);
        REQUIRE(parsed.has_value());
        const auto& profile = parsed.value();
        REQUIRE_FALSE(profile.diagnostics.empty());
        CHECK(profile.diagnostics.front().line != 0U);
        CHECK(profile.settings.input == program::defaults());
        CHECK(required_value(profile.settings.effects.left_trigger).mode ==
              output::trigger_mode::feedback);
    }
}

TEST_CASE("profiles.input_preserves_tiny_positive_thresholds", "[unit][profiles]") {
    const auto parsed = parse_profile(R"(
[input]
remap = [{ from = "left_trigger", to = "left_shoulder", threshold = 1e-50 }]
)");
    REQUIRE(parsed.has_value());
    CHECK(parsed->diagnostics.empty());
    CHECK(parsed->settings.input.mapping(source::left_trigger).threshold == 1.0e-50);

    auto state = project(*parsed, 0U, touch_source::none, 0.0F);
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::left_shoulder)) == 0U);
    state = project(*parsed, 0U, touch_source::none, std::numeric_limits<float>::denorm_min());
    CHECK((state.buttons & static_cast<button_mask>(gamepad_button::left_shoulder)) != 0U);
}
