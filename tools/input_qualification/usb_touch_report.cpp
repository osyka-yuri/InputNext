#include "usb_touch_report.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::input_qualification::diagnostic {
namespace {

constexpr std::size_t click_offset = 10U;
constexpr std::uint8_t click_mask = 0x02U;
constexpr std::array<std::size_t, 2U> contact_offsets{33U, 37U};
constexpr std::uint8_t inactive_mask = 0x80U;
constexpr std::uint8_t contact_id_mask = 0x7FU;
constexpr std::uint8_t coordinate_low_nibble_mask = 0x0FU;

[[nodiscard]] constexpr std::uint16_t decode_x(const std::span<const std::uint8_t> packet,
    const std::size_t offset) noexcept {
    const auto low = static_cast<std::uint16_t>(packet[offset + 1U]);
    const auto high = static_cast<std::uint16_t>(packet[offset + 2U] & coordinate_low_nibble_mask);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

[[nodiscard]] constexpr std::uint16_t decode_y(const std::span<const std::uint8_t> packet,
    const std::size_t offset) noexcept {
    const auto low = static_cast<std::uint16_t>(packet[offset + 2U] >> 4U);
    const auto high = static_cast<std::uint16_t>(packet[offset + 3U]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 4U));
}

} // namespace

std::optional<dualsense_usb_touch_contacts> decode_dualsense_usb_touch(
    const std::span<const std::uint8_t> packet,
    const bool observed_click) noexcept {
    if (packet.size() != dualsense_usb_input_report_size ||
        packet[0U] != dualsense_usb_input_report_id ||
        ((packet[click_offset] & click_mask) != 0U) != observed_click) {
        return std::nullopt;
    }

    dualsense_usb_touch_contacts result{};
    for (const std::size_t offset : contact_offsets) {
        const std::uint8_t packed_id = packet[offset];
        if ((packed_id & inactive_mask) != 0U) {
            continue;
        }

        const auto id = static_cast<std::uint64_t>(packed_id & contact_id_mask);
        const auto x = decode_x(packet, offset);
        const auto y = decode_y(packet, offset);
        if (x >= dualsense_usb_touch_width || y >= dualsense_usb_touch_height) {
            return std::nullopt;
        }
        for (std::size_t previous = 0U; previous < result.count; ++previous) {
            if (result.contacts[previous].id == id) {
                return std::nullopt;
            }
        }

        result.contacts[result.count] = input::touch_contact{
            .id = id,
            .x = static_cast<double>(x),
            .y = static_cast<double>(y),
            .active = true,
        };
        ++result.count;
    }
    return result;
}

} // namespace inputnext::input_qualification::diagnostic
