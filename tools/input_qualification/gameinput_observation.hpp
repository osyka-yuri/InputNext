#pragma once

#include <GameInput.h>

#include "report.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace inputnext::input {
class touch_resolver;
}

namespace inputnext::input_qualification::diagnostic {

namespace gameinput = GameInput::v4;

inline constexpr auto observation_kinds = static_cast<gameinput::GameInputKind>(
    gameinput::GameInputKindGamepad | gameinput::GameInputKindControllerButton |
    gameinput::GameInputKindTouch);
inline constexpr std::uint32_t max_button_count = 64U;
inline constexpr auto max_touch_surfaces = static_cast<std::uint32_t>(reading_surface_capacity);
inline constexpr auto max_contacts_per_surface =
    static_cast<std::uint32_t>(reading_contact_capacity);
inline constexpr std::uint32_t max_input_report_descriptors = 16U;
inline constexpr std::uint32_t dualsense_usb_click_button = 13U;
inline constexpr std::uint32_t dualsense_usb_button_count = 15U;

struct device_report final {
    std::uint16_t vendor{};
    std::uint16_t product{};
    std::uint16_t revision{};
    std::uint32_t supported_input{};
    gameinput::GameInputVersion hardware_version{};
    gameinput::GameInputVersion firmware_version{};
    std::string display_name;
    std::string pnp_path;
    std::string device_id;
    std::array<gameinput::GameInputLabel, max_button_count> labels{};
    std::uint32_t button_count{};
    std::uint32_t labels_returned{};
    std::array<gameinput::GameInputTouchSurfaceInfo, max_touch_surfaces> surfaces{};
    std::array<bool, max_touch_surfaces> surface_info_available{};
    std::uint32_t surface_count{};
    bool button_count_truncated{};
    bool surface_count_truncated{};
    std::uint32_t input_report_count{};
    std::array<gameinput::GameInputRawDeviceReportInfo, max_input_report_descriptors>
        input_reports{};
    std::uint32_t returned_input_report_count{};
    bool input_report_count_truncated{};
    bool raw_usb_mode_requested{};
    bool raw_usb_mode_enabled{};
    std::uint32_t matching_raw_input_descriptor_count{};
};

[[nodiscard]] device_report inspect_device(gameinput::IGameInputDevice& device,
    bool raw_usb_requested);

// Each stream can resolve click sides independently. The reverse tail has no resolver.
[[nodiscard]] reading_sample inspect_reading(gameinput::IGameInputReading& reading,
    const device_report& device,
    input::touch_resolver* resolver) noexcept;

} // namespace inputnext::input_qualification::diagnostic
