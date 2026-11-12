#pragma once

#include "gameinput/input_profile.hpp"
#include <inputnext/device_metadata.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {

struct input_version final {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t build{};
    std::uint16_t revision{};

    friend constexpr bool operator==(const input_version&, const input_version&) noexcept = default;
};

struct input_report_shape final {
    std::uint32_t id{};
    std::uint32_t size{};
};

struct input_qualification_facts final {
    metadata_field<input_version> runtime_version{};
    metadata_field<input_version> hardware_version{};
    metadata_field<input_version> firmware_version{};
    metadata_field<std::uint16_t> vendor_id{};
    metadata_field<std::uint16_t> product_id{};
    metadata_field<std::uint16_t> revision{};
    std::uint32_t supported_input_bits{};
    bool gamepad_supported{};
    bool controller_button_supported{};
    std::uint32_t button_count{};
    bool button_labels_complete{};
    std::span<const std::uint32_t> button_labels;
    std::uint32_t touch_surface_count{};
    metadata_field<transport_metadata> transport{};
    std::optional<input_report_shape> single_input_report = std::nullopt;
};

// Returns a conservative connection profile. Unknown or unqualified combinations stay on
// ordinary gamepad history; only a source-owned compile-time policy row can enable touch.
[[nodiscard]] native_input_profile qualify_native_input(
    const input_qualification_facts& facts) noexcept;

} // namespace inputnext::gameinput_detail
