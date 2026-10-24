#include "usb_report.hpp"

#include "feedback_payload.hpp"
#include <inputnext/output.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail::dualsense {
namespace {

constexpr std::size_t report_id_offset = 0U;

constexpr std::uint8_t report_id = 0x02U;

[[nodiscard]] constexpr std::byte as_byte(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

} // namespace

usb_report encode_usb_report(const output::channel_values& values) noexcept {
    usb_report report{};
    report[report_id_offset] = as_byte(report_id);
    const feedback_payload payload = encode_feedback_payload(values);
    std::ranges::copy(payload, report.begin() + 1);
    return report;
}

std::optional<std::span<const std::byte>> usb_report_payload(const usb_report& report,
    std::size_t native_capacity) noexcept {
    const std::span<const std::byte> bytes{report};
    switch (native_capacity) {
    case 64U:
        return bytes;
    case 63U:
        return bytes.subspan(1U);
    case 48U:
        return bytes.first(48U);
    default:
        return std::nullopt;
    }
}

} // namespace inputnext::gameinput_detail::dualsense
