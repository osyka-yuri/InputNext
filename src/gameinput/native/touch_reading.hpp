#pragma once

#include "dualsense_usb_touch_reading.hpp"
#include "gameinput/dualsense/usb_touch_report.hpp"
#include "gameinput/input_profile.hpp"
#include "input/touch_resolver.hpp"

#include <GameInput.h>

#include <array>
#include <cstdint>

namespace inputnext::gameinput_detail {

namespace touch_reading_detail {

inline void read_surface_contacts(GameInput::v4::IGameInputReading& reading,
    const native_input_profile& profile,
    input::touch_observation& touch) noexcept {
    if (profile.surface_width == 0U || profile.surface_height == 0U) {
        return;
    }
    std::array<GameInput::v4::GameInputTouchContact, 2U> contacts{};
    const auto count = reading.GetTouchContactCount(profile.surface_index);
    if (count > contacts.size() || reading.GetTouchSurfaceState(profile.surface_index,
                                       count,
                                       count == 0U ? nullptr : contacts.data()) != count) {
        return;
    }

    touch.contacts_status = input::observation_status::observed;
    touch.contacts_complete = true;
    touch.surface_width = static_cast<double>(profile.surface_width);
    touch.surface_height = static_cast<double>(profile.surface_height);
    touch.contact_count = count;
    for (std::uint32_t index = 0U; index < count; ++index) {
        touch.contacts[index] = input::touch_contact{
            .id = contacts[index].contactId,
            .x = static_cast<double>(contacts[index].padX),
            .y = static_cast<double>(contacts[index].padY),
            .active = true,
        };
    }
}

inline void read_coordinates(GameInput::v4::IGameInputReading& reading,
    const native_input_profile& profile,
    input::touch_observation& touch) noexcept {
    if (profile.coordinates == touch_coordinate_source::none) {
        touch.contacts_status = input::observation_status::unsupported;
        return;
    }
    // A release never selects a side. Pressed readings remain self-contained for replay.
    if (!touch.click_pressed) {
        touch.contacts_status = input::observation_status::not_applicable;
        return;
    }
    if (profile.coordinates == touch_coordinate_source::surface) {
        read_surface_contacts(reading, profile, touch);
        return;
    }

    if (profile.coordinates == touch_coordinate_source::dualsense_usb_report) {
        const auto contacts = read_dualsense_usb_touch(reading, touch.click_pressed);
        if (contacts.has_value()) {
            touch.contacts_status = input::observation_status::observed;
            touch.contacts_complete = true;
            touch.surface_width = dualsense_usb_touch_width;
            touch.surface_height = dualsense_usb_touch_height;
            touch.contacts = contacts->contacts;
            touch.contact_count = contacts->count;
        }
    }
}

} // namespace touch_reading_detail

// Read click and optional coordinates from one retained GameInput reading. Missing or malformed
// optional coordinates do not invalidate an otherwise observed mechanical click.
[[nodiscard]] inline input::touch_observation read_touch_observation(
    GameInput::v4::IGameInputReading& reading,
    const native_input_profile& profile) noexcept {
    constexpr std::uint32_t button_capacity = 64U;

    input::touch_observation touch{};
    constexpr auto required_kinds =
        static_cast<std::uint32_t>(GameInput::v4::GameInputKindGamepad) |
        static_cast<std::uint32_t>(GameInput::v4::GameInputKindControllerButton) |
        static_cast<std::uint32_t>(GameInput::v4::GameInputKindTouch);
    if ((static_cast<std::uint32_t>(reading.GetInputKind()) & required_kinds) == 0U) {
        touch.click_status = input::observation_status::not_applicable;
        touch.contacts_status = input::observation_status::not_applicable;
        return touch;
    }
    touch.click_status = input::observation_status::unavailable;
    touch.contacts_status = input::observation_status::unavailable;
    if (!profile.touch_enabled() || profile.button_count == 0U ||
        profile.button_count > button_capacity || profile.click_button >= profile.button_count) {
        return touch;
    }

    const auto button_count = reading.GetControllerButtonCount();
    if (button_count != profile.button_count) {
        return touch;
    }
    std::array<bool, button_capacity> button_states{};
    if (reading.GetControllerButtonState(button_count, button_states.data()) != button_count) {
        return touch;
    }

    touch.click_status = input::observation_status::observed;
    touch.click_pressed = button_states[profile.click_button];
    touch.same_reading = true;
    touch.source_timestamp = reading.GetTimestamp();
    touch_reading_detail::read_coordinates(reading, profile, touch);
    return touch;
}

} // namespace inputnext::gameinput_detail
