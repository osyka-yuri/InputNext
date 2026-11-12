#pragma once

#include "input/program.hpp"
#include "input/remappable_buttons.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace inputnext::profiles::detail {

struct control_name final {
    std::string_view name;
    input::source control;
};

struct control_layout final {
    std::string_view name;
    std::span<const control_name> aliases;
};

inline constexpr std::array universal_extra_controls{
    control_name{.name = "left_trigger", .control = input::source::left_trigger},
    control_name{.name = "right_trigger", .control = input::source::right_trigger},
    control_name{.name = "touchpad_left", .control = input::source::touch_left},
    control_name{.name = "touchpad_right", .control = input::source::touch_right},
    control_name{.name = "touchpad_unknown_side", .control = input::source::touch_fallback},
};

inline constexpr std::array playstation_control_names{
    control_name{.name = "cross", .control = input::source::face_bottom},
    control_name{.name = "circle", .control = input::source::face_right},
    control_name{.name = "square", .control = input::source::face_left},
    control_name{.name = "triangle", .control = input::source::face_top},
    control_name{.name = "create", .control = input::source::select},
    control_name{.name = "options", .control = input::source::menu},
    control_name{.name = "l1", .control = input::source::left_shoulder},
    control_name{.name = "r1", .control = input::source::right_shoulder},
    control_name{.name = "l2", .control = input::source::left_trigger},
    control_name{.name = "r2", .control = input::source::right_trigger},
    control_name{.name = "l3", .control = input::source::left_stick_click},
    control_name{.name = "r3", .control = input::source::right_stick_click},
};

inline constexpr std::array xbox_control_names{
    control_name{.name = "a", .control = input::source::face_bottom},
    control_name{.name = "b", .control = input::source::face_right},
    control_name{.name = "x", .control = input::source::face_left},
    control_name{.name = "y", .control = input::source::face_top},
    control_name{.name = "view", .control = input::source::select},
    control_name{.name = "lb", .control = input::source::left_shoulder},
    control_name{.name = "rb", .control = input::source::right_shoulder},
    control_name{.name = "lt", .control = input::source::left_trigger},
    control_name{.name = "rt", .control = input::source::right_trigger},
    control_name{.name = "ls", .control = input::source::left_stick_click},
    control_name{.name = "rs", .control = input::source::right_stick_click},
};

inline constexpr control_layout universal_layout{.name = "universal", .aliases = {}};
inline constexpr std::array control_layouts{
    universal_layout,
    control_layout{.name = "playstation", .aliases = playstation_control_names},
    control_layout{.name = "xbox", .aliases = xbox_control_names},
};

[[nodiscard]] constexpr const control_layout* find_layout(const std::string_view name) noexcept {
    const auto layout = std::ranges::find(control_layouts, name, &control_layout::name);
    return layout != control_layouts.end() ? &*layout : nullptr;
}

[[nodiscard]] constexpr std::optional<input::source> resolve_control(const std::string_view name,
    const control_layout& layout) noexcept {
    const auto button =
        std::ranges::find(input::remappable_buttons, name, &input::remappable_button::name);
    if (button != input::remappable_buttons.end()) {
        return button->physical;
    }
    const auto extra = std::ranges::find(universal_extra_controls, name, &control_name::name);
    if (extra != universal_extra_controls.end()) {
        return extra->control;
    }
    const auto alias = std::ranges::find(layout.aliases, name, &control_name::name);
    return alias != layout.aliases.end() ? std::optional{alias->control} : std::nullopt;
}

} // namespace inputnext::profiles::detail
