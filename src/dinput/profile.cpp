#include "profile.hpp"

#include "native_types.hpp"

#include <inputnext/gamepad_state.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace inputnext::dinput {
namespace {

constexpr WORD generic_desktop_page = 0x01U;
constexpr WORD button_page = 0x09U;
constexpr DWORD max_property_percent = 10000U;

[[nodiscard]] constexpr GUID object_guid(DWORD data1) noexcept {
    return GUID{.Data1 = data1,
        .Data2 = 0xC9F3U,
        .Data3 = 0x11CFU,
        .Data4 = {0xBFU, 0xC7U, 0x44U, 0x45U, 0x53U, 0x54U, 0x00U, 0x00U}};
}

[[nodiscard]] constexpr object_descriptor axis_descriptor(std::size_t index,
    DWORD guid_data1,
    DWORD offset,
    WORD usage,
    const wchar_t* name) noexcept {
    return object_descriptor{.guid = object_guid(guid_data1),
        .id = static_cast<DWORD>(DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(static_cast<WORD>(index))),
        .native_offset = offset,
        .usage_page = generic_desktop_page,
        .usage = usage,
        .name = name,
        .kind = object_kind::axis,
        .source_index = index};
}

[[nodiscard]] constexpr object_descriptor button_descriptor(std::size_t index,
    const wchar_t* name) noexcept {
    return object_descriptor{.guid = object_guid(0xA36D02F0U),
        .id = static_cast<DWORD>(DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(static_cast<WORD>(index))),
        .native_offset = static_cast<DWORD>(offsetof(DIJOYSTATE, rgbButtons) + index),
        .usage_page = button_page,
        .usage = static_cast<WORD>(index + 1U),
        .name = name,
        .kind = object_kind::button,
        .source_index = index};
}

constexpr std::array profile_objects{
    axis_descriptor(0U,
        0xA36D02E0U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lX)),
        0x30U,
        L"X Axis"),
    axis_descriptor(1U,
        0xA36D02E1U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lY)),
        0x31U,
        L"Y Axis"),
    axis_descriptor(2U,
        0xA36D02E2U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lZ)),
        0x32U,
        L"Left Trigger"),
    axis_descriptor(3U,
        0xA36D02F4U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lRx)),
        0x33U,
        L"Rx Axis"),
    axis_descriptor(4U,
        0xA36D02F5U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lRy)),
        0x34U,
        L"Ry Axis"),
    axis_descriptor(5U,
        0xA36D02E3U,
        static_cast<DWORD>(offsetof(DIJOYSTATE, lRz)),
        0x35U,
        L"Right Trigger"),
    button_descriptor(0U, L"South"),
    button_descriptor(1U, L"East"),
    button_descriptor(2U, L"West"),
    button_descriptor(3U, L"North"),
    button_descriptor(4U, L"Left Shoulder"),
    button_descriptor(5U, L"Right Shoulder"),
    button_descriptor(6U, L"View"),
    button_descriptor(7U, L"Menu"),
    button_descriptor(8U, L"Left Stick"),
    button_descriptor(9U, L"Right Stick"),
    button_descriptor(10U, L"Guide"),
    button_descriptor(11U, L"D-pad Up"),
    button_descriptor(12U, L"D-pad Down"),
    button_descriptor(13U, L"D-pad Left"),
    button_descriptor(14U, L"D-pad Right"),
    object_descriptor{.guid = object_guid(0xA36D02F2U),
        .id = static_cast<DWORD>(DIDFT_POV | DIDFT_MAKEINSTANCE(0U)),
        .native_offset = static_cast<DWORD>(offsetof(DIJOYSTATE, rgdwPOV)),
        .usage_page = generic_desktop_page,
        .usage = 0x39U,
        .name = L"POV",
        .kind = object_kind::pov,
        .source_index = 0U},
};
static_assert(profile_objects.size() == normalized_object_count);

[[nodiscard]] constexpr LONG neutral_stick(const axis_properties& properties) noexcept {
    if (properties.maximum <= properties.minimum) {
        return properties.minimum;
    }
    const auto minimum = static_cast<std::int64_t>(properties.minimum);
    const auto maximum = static_cast<std::int64_t>(properties.maximum);
    return static_cast<LONG>(minimum + ((maximum - minimum) / 2));
}

struct axis_range final {
    LONG minimum{};
    LONG maximum{};
};

[[nodiscard]] LONG rounded_axis_value(double value, axis_range range) noexcept {
    const auto rounded = static_cast<std::int64_t>(std::llround(value));
    const auto bounded = std::clamp(rounded,
        static_cast<std::int64_t>(range.minimum),
        static_cast<std::int64_t>(range.maximum));
    return static_cast<LONG>(bounded);
}

[[nodiscard]] double deadzone_fraction(DWORD value) noexcept {
    return static_cast<double>(std::min(value, max_property_percent)) /
           static_cast<double>(max_property_percent);
}

[[nodiscard]] double saturation_fraction(DWORD value) noexcept {
    const auto bounded = std::min(value, max_property_percent);
    return static_cast<double>(bounded) / static_cast<double>(max_property_percent);
}

[[nodiscard]] LONG project_stick(float raw, const axis_properties& properties) noexcept {
    if (properties.maximum <= properties.minimum) {
        return properties.minimum;
    }

    const auto center = neutral_stick(properties);
    const auto deadzone = deadzone_fraction(properties.deadzone);
    const auto saturation = saturation_fraction(properties.saturation);
    const auto magnitude = std::abs(static_cast<double>(std::clamp(raw, -1.0F, 1.0F)));
    if (magnitude <= deadzone) {
        return center;
    }

    const auto saturation_point = deadzone + ((1.0 - deadzone) * saturation);
    if (magnitude >= saturation_point) {
        return raw < 0.0F ? properties.minimum : properties.maximum;
    }
    const auto scaled = (magnitude - deadzone) / (saturation_point - deadzone);
    const auto direction = raw < 0.0F ? -1.0 : 1.0;
    const auto extent = raw < 0.0F ? static_cast<double>(center) - properties.minimum
                                   : static_cast<double>(properties.maximum) - center;
    return rounded_axis_value(static_cast<double>(center) + (direction * scaled * extent),
        axis_range{.minimum = properties.minimum, .maximum = properties.maximum});
}

[[nodiscard]] LONG project_trigger(float raw, const axis_properties& properties) noexcept {
    if (properties.maximum <= properties.minimum) {
        return properties.minimum;
    }

    const auto deadzone = deadzone_fraction(properties.deadzone);
    const auto saturation = saturation_fraction(properties.saturation);
    const auto magnitude = static_cast<double>(std::clamp(raw, 0.0F, 1.0F));
    if (magnitude <= deadzone) {
        return properties.minimum;
    }
    const auto saturation_point = deadzone + ((1.0 - deadzone) * saturation);
    if (magnitude >= saturation_point) {
        return properties.maximum;
    }

    const auto scaled = (magnitude - deadzone) / (saturation_point - deadzone);
    const auto range = static_cast<double>(properties.maximum) - properties.minimum;
    return rounded_axis_value(static_cast<double>(properties.minimum) + (scaled * range),
        axis_range{.minimum = properties.minimum, .maximum = properties.maximum});
}

[[nodiscard]] bool pressed(const gamepad_state& state, gamepad_button button) noexcept {
    return (state.buttons & static_cast<button_mask>(button)) != 0U;
}

[[nodiscard]] DWORD project_pov(const gamepad_state& state) noexcept {
    const auto up_pressed = pressed(state, gamepad_button::dpad_up);
    const auto down_pressed = pressed(state, gamepad_button::dpad_down);
    const auto left_pressed = pressed(state, gamepad_button::dpad_left);
    const auto right_pressed = pressed(state, gamepad_button::dpad_right);
    const auto vertical_axis_up = up_pressed != down_pressed && up_pressed;
    const auto vertical_axis_down = up_pressed != down_pressed && down_pressed;
    const auto horizontal_axis_left = left_pressed != right_pressed && left_pressed;
    const auto horizontal_axis_right = left_pressed != right_pressed && right_pressed;

    if (vertical_axis_up) {
        if (horizontal_axis_right) {
            return 4500U;
        }
        return horizontal_axis_left ? 31500U : 0U;
    }
    if (horizontal_axis_right) {
        return vertical_axis_down ? 13500U : 9000U;
    }
    if (vertical_axis_down) {
        return horizontal_axis_left ? 22500U : 18000U;
    }
    return horizontal_axis_left ? 27000U : 0xffffffffU;
}

} // namespace

std::span<const object_descriptor> objects() noexcept {
    return profile_objects;
}

profile_values project(const gamepad_state& source,
    const std::array<axis_properties, axis_count>& properties) noexcept {
    profile_values result{};
    for (std::size_t index = 0U; index < result.axes.size(); ++index) {
        result.axes[index] = index == 2U || index == 5U ? properties[index].minimum
                                                        : neutral_stick(properties[index]);
    }
    if (!is_valid_gamepad_state(source)) {
        return result;
    }

    const std::array<float, 6> source_axes{source.left_stick.x,
        -source.left_stick.y,
        source.left_trigger,
        source.right_stick.x,
        -source.right_stick.y,
        source.right_trigger};
    for (std::size_t index = 0U; index < result.axes.size(); ++index) {
        result.axes[index] = index == 2U || index == 5U
                                 ? project_trigger(source_axes[index], properties[index])
                                 : project_stick(source_axes[index], properties[index]);
    }

    constexpr std::array<gamepad_button, 15> button_order{gamepad_button::south,
        gamepad_button::east,
        gamepad_button::west,
        gamepad_button::north,
        gamepad_button::left_shoulder,
        gamepad_button::right_shoulder,
        gamepad_button::view,
        gamepad_button::menu,
        gamepad_button::left_stick,
        gamepad_button::right_stick,
        gamepad_button::guide,
        gamepad_button::dpad_up,
        gamepad_button::dpad_down,
        gamepad_button::dpad_left,
        gamepad_button::dpad_right};
    for (std::size_t index = 0U; index < button_order.size(); ++index) {
        result.buttons[index] = pressed(source, button_order[index]) ? 0x80U : 0U;
    }
    result.pov = project_pov(source);
    return result;
}

} // namespace inputnext::dinput
