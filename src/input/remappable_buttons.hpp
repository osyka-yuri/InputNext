#pragma once

#include "program.hpp"

#include <inputnext/gamepad_state.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace inputnext::input {

struct remappable_button final {
    source physical;
    gamepad_button button;
    std::string_view name;
};

// This neutral registry owns the ordinary source-to-mask relationship used by projection and
// profile parsing. Controller-family aliases belong to profile parsing only.
inline constexpr auto remappable_buttons = std::to_array<remappable_button>({
    {.physical = source::dpad_up, .button = gamepad_button::dpad_up, .name = "dpad_up"},
    {.physical = source::dpad_down, .button = gamepad_button::dpad_down, .name = "dpad_down"},
    {.physical = source::dpad_left, .button = gamepad_button::dpad_left, .name = "dpad_left"},
    {.physical = source::dpad_right, .button = gamepad_button::dpad_right, .name = "dpad_right"},
    {.physical = source::menu, .button = gamepad_button::menu, .name = "menu"},
    {.physical = source::select, .button = gamepad_button::view, .name = "select"},
    {.physical = source::left_stick_click,
        .button = gamepad_button::left_stick,
        .name = "left_stick_click"},
    {.physical = source::right_stick_click,
        .button = gamepad_button::right_stick,
        .name = "right_stick_click"},
    {.physical = source::left_shoulder,
        .button = gamepad_button::left_shoulder,
        .name = "left_shoulder"},
    {.physical = source::right_shoulder,
        .button = gamepad_button::right_shoulder,
        .name = "right_shoulder"},
    {.physical = source::face_bottom, .button = gamepad_button::south, .name = "face_bottom"},
    {.physical = source::face_right, .button = gamepad_button::east, .name = "face_right"},
    {.physical = source::face_left, .button = gamepad_button::west, .name = "face_left"},
    {.physical = source::face_top, .button = gamepad_button::north, .name = "face_top"},
});

// The projector derives a compact lookup indexed by ordinary button source.
static_assert(remappable_buttons.size() == static_cast<std::size_t>(source::left_trigger));
static_assert([] {
    for (std::size_t index = 0U; index < remappable_buttons.size(); ++index) {
        if (static_cast<std::size_t>(remappable_buttons[index].physical) != index) {
            return false;
        }
    }
    return true;
}());

} // namespace inputnext::input
