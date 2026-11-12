#include "program.hpp"
#include "remappable_buttons.hpp"

#include <inputnext/gamepad_state.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace inputnext::input {
namespace {

constexpr auto ordinary_buttons = []<std::size_t... Index>(std::index_sequence<Index...>) {
    return std::array{remappable_buttons[Index].button...};
}(std::make_index_sequence<remappable_buttons.size()>{});

[[nodiscard]] constexpr std::size_t index_of(const source input) noexcept {
    return static_cast<std::size_t>(input);
}

[[nodiscard]] constexpr bool is_button_source(const source input) noexcept {
    return index_of(input) < ordinary_buttons.size() || input == source::touch_left ||
           input == source::touch_right || input == source::touch_fallback;
}

[[nodiscard]] constexpr bool valid_target_button(const gamepad_button button) noexcept {
    return std::ranges::find(ordinary_buttons, button) != ordinary_buttons.end();
}

[[nodiscard]] constexpr bool is_pressed(const physical_frame& frame, const source input) noexcept {
    if (input == source::touch_left) {
        return frame.touch == touch_source::left;
    }
    if (input == source::touch_right) {
        return frame.touch == touch_source::right;
    }
    if (input == source::touch_fallback) {
        return frame.touch == touch_source::fallback;
    }
    const auto position = index_of(input);
    return position < ordinary_buttons.size() &&
           (frame.pad.buttons & static_cast<button_mask>(ordinary_buttons[position])) != 0U;
}

[[nodiscard]] constexpr float value_of(const physical_frame& frame, const source input) noexcept {
    if (input == source::left_trigger) {
        return frame.pad.left_trigger;
    }
    if (input == source::right_trigger) {
        return frame.pad.right_trigger;
    }
    return is_pressed(frame, input) ? 1.0F : 0.0F;
}

[[nodiscard]] constexpr bool identity_route(const source input, const route& mapping) noexcept {
    if (input == source::left_trigger) {
        return mapping.kind == target_kind::trigger && !mapping.right_trigger;
    }
    if (input == source::right_trigger) {
        return mapping.kind == target_kind::trigger && mapping.right_trigger;
    }
    if (input == source::touch_left || input == source::touch_right ||
        input == source::touch_fallback) {
        return mapping.kind == target_kind::disabled;
    }
    if (!is_button_source(input)) {
        return false;
    }
    const auto index = index_of(input);
    return mapping.kind == target_kind::button && mapping.button == ordinary_buttons[index];
}

[[nodiscard]] bool valid_mapping(const source input, const route& mapping) noexcept {
    if (index_of(input) >= index_of(source::count)) {
        return false;
    }
    if (mapping.kind == target_kind::disabled) {
        return !mapping.button.has_value() && !mapping.right_trigger && mapping.threshold == 0.5;
    }
    if (mapping.kind == target_kind::button) {
        return !mapping.right_trigger && mapping.button.has_value() &&
               valid_target_button(*mapping.button) && std::isfinite(mapping.threshold) &&
               mapping.threshold > 0.0F && mapping.threshold <= 1.0F &&
               (is_trigger_source(input) || mapping.threshold == 0.5);
    }
    return mapping.kind == target_kind::trigger && !mapping.button.has_value() &&
           std::isfinite(mapping.threshold) && mapping.threshold == 0.5;
}

} // namespace

program::program() noexcept {
    for (std::size_t index = 0U; index < ordinary_buttons.size(); ++index) {
        routes_[index] = route{.kind = target_kind::button, .button = ordinary_buttons[index]};
    }
    routes_[index_of(source::left_trigger)] =
        route{.kind = target_kind::trigger, .right_trigger = false};
    routes_[index_of(source::right_trigger)] =
        route{.kind = target_kind::trigger, .right_trigger = true};
}

program program::identity() noexcept {
    return program{};
}

program program::defaults() noexcept {
    program value;
    value.routes_[index_of(source::touch_left)] =
        route{.kind = target_kind::button, .button = gamepad_button::view};
    value.routes_[index_of(source::touch_right)] =
        route{.kind = target_kind::button, .button = gamepad_button::menu};
    value.routes_[index_of(source::touch_fallback)] =
        route{.kind = target_kind::button, .button = gamepad_button::view};
    value.touch_routes_ = true;
    value.identity_ = false;
    value.ordinary_identity_ = true;
    return value;
}

bool program::has_touch_routes() const noexcept {
    return touch_routes_;
}

bool program::is_identity() const noexcept {
    return identity_;
}

bool program::is_ordinary_identity() const noexcept {
    return ordinary_identity_;
}

const route& program::mapping(const source input) const noexcept {
    const auto index = index_of(input);
    if (index >= routes_.size()) {
        return routes_[0U];
    }
    return routes_[index];
}

program_builder::program_builder(program base) noexcept : value_(base) {
}

bool program_builder::set(const source input, const route output) noexcept {
    if (!valid_mapping(input, output)) {
        valid_ = false;
        return false;
    }
    value_.routes_[index_of(input)] = output;
    return true;
}

std::optional<program> program_builder::build() const noexcept {
    if (!valid_) {
        return std::nullopt;
    }
    program result = value_;
    result.touch_routes_ = false;
    result.identity_ = true;
    result.ordinary_identity_ = true;
    for (std::size_t index = 0U; index < result.routes_.size(); ++index) {
        const auto input = static_cast<source>(index);
        const auto& mapping = result.routes_[index];
        if (input == source::touch_left || input == source::touch_right ||
            input == source::touch_fallback) {
            result.touch_routes_ |= mapping.kind != target_kind::disabled;
        } else {
            result.ordinary_identity_ &= identity_route(input, mapping);
        }
        result.identity_ &= identity_route(input, mapping);
    }
    return result;
}

gamepad_state project(const physical_frame& frame, const program& mappings) noexcept {
    if (mappings.is_ordinary_identity() && frame.touch == touch_source::none) {
        return frame.pad;
    }

    gamepad_state result{
        .buttons = frame.pad.buttons & static_cast<button_mask>(gamepad_button::guide),
        .left_stick = frame.pad.left_stick,
        .right_stick = frame.pad.right_stick,
    };
    for (std::size_t index = 0U; index < static_cast<std::size_t>(source::count); ++index) {
        const auto input = static_cast<source>(index);
        const route& mapping = mappings.mapping(input);
        if (mapping.kind == target_kind::disabled) {
            continue;
        }
        const float value = value_of(frame, input);
        if (mapping.kind == target_kind::button) {
            const bool active = is_trigger_source(input)
                                    ? static_cast<double>(value) >= mapping.threshold
                                    : value > 0.0F;
            if (active && mapping.button.has_value()) {
                result.buttons |= static_cast<button_mask>(*mapping.button);
            }
            continue;
        }
        float& trigger = mapping.right_trigger ? result.right_trigger : result.left_trigger;
        trigger = trigger < value ? value : trigger;
    }
    return result;
}

} // namespace inputnext::input
