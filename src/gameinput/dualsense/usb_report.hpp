#pragma once

#include <inputnext/output.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail::dualsense {

using usb_report = std::array<std::byte, 64U>;

[[nodiscard]] usb_report encode_usb_report(const output::channel_values& values) noexcept;

[[nodiscard]] std::optional<std::span<const std::byte>> usb_report_payload(const usb_report& report,
    std::size_t native_capacity) noexcept;

} // namespace inputnext::gameinput_detail::dualsense
