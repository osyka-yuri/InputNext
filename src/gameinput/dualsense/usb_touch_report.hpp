#pragma once

#include "input/touch_resolver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {

inline constexpr std::uint32_t dualsense_usb_input_report_id = 1U;
inline constexpr std::size_t dualsense_usb_input_report_size = 64U;
inline constexpr std::uint32_t dualsense_usb_touch_width = 1920U;
inline constexpr std::uint32_t dualsense_usb_touch_height = 1080U;

struct dualsense_usb_touch_contacts final {
    std::array<input::touch_contact, 2U> contacts{};
    std::size_t count{};
};

// Decode the exact USB input report and verify that its raw click bit agrees with the
// controller-button state read from the same GameInput reading. An engaged empty result means
// that the complete report contained no active contacts.
[[nodiscard]] std::optional<dualsense_usb_touch_contacts>
decode_dualsense_usb_touch(std::span<const std::uint8_t> packet, bool observed_click) noexcept;

} // namespace inputnext::gameinput_detail
