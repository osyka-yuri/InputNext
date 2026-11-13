#include <GameInput.h>
#include <Windows.h>

#include "gameinput_observation.hpp"

#include "gameinput/dualsense/usb_touch_report.hpp"
#include "gameinput/native/dualsense_usb_touch_reading.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "report.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>

namespace inputnext::input_qualification::diagnostic {
namespace {

template <std::size_t Size, typename Getter>
[[nodiscard]] std::string read_device_string(gameinput::IGameInputDevice& device, Getter getter) {
    std::array<char, Size> buffer{};
    const auto returned =
        (device.*getter)(static_cast<std::uint32_t>(buffer.size()), buffer.data());
    const auto length = std::min<std::size_t>(returned, buffer.size() - 1U);
    return std::string{buffer.data(), length};
}

[[nodiscard]] std::string format_device_id(gameinput::IGameInputDevice& device) {
    APP_LOCAL_DEVICE_ID id{};
    device.GetDeviceId(&id);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (std::size_t index = 0U; index < sizeof(id); ++index) {
        text << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return text.str();
}

void inspect_touch_surfaces(gameinput::IGameInputDevice& device, device_report& report) noexcept {
    const auto surfaces = device.GetTouchSurfaceCount();
    report.surface_count = std::min(surfaces, max_touch_surfaces);
    report.surface_count_truncated = surfaces > max_touch_surfaces;
    for (std::uint32_t index = 0U; index < report.surface_count; ++index) {
        report.surface_info_available[index] =
            SUCCEEDED(device.GetTouchSurfaceInfo(index, &report.surfaces[index]));
    }
}

void inspect_raw_report_shape(gameinput::IGameInputDevice& device, device_report& report) noexcept {
    report.input_report_count = device.GetInputReportCount();
    const auto bounded = std::min(report.input_report_count, max_input_report_descriptors);
    report.input_report_count_truncated = report.input_report_count > bounded;
    if (bounded == 0U) {
        return;
    }

    const auto returned = device.GetInputReportInfo(bounded, report.input_reports.data());
    report.input_report_count_truncated = report.input_report_count_truncated || returned > bounded;
    report.returned_input_report_count = std::min(returned, bounded);
    for (std::uint32_t index = 0U; index < report.returned_input_report_count; ++index) {
        const auto& descriptor = report.input_reports[index];
        const bool matches = descriptor.kind == gameinput::GameInputRawInputReport &&
                             descriptor.id == gameinput_detail::dualsense_usb_input_report_id &&
                             descriptor.size == gameinput_detail::dualsense_usb_input_report_size;
        report.matching_raw_input_descriptor_count += matches ? 1U : 0U;
    }
    report.raw_usb_mode_enabled =
        report.button_count == dualsense_usb_button_count && !report.button_count_truncated &&
        report.input_report_count == 1U && report.returned_input_report_count == 1U &&
        !report.input_report_count_truncated && report.matching_raw_input_descriptor_count == 1U;
}

void inspect_buttons(gameinput::IGameInputReading& reading, reading_sample& sample) noexcept {
    const auto raw_count = reading.GetControllerButtonCount();
    sample.reading_button_count = std::min(raw_count, max_button_count);
    sample.button_count_truncated = raw_count > max_button_count;
    std::array<bool, max_button_count> states{};
    sample.returned_button_count =
        std::min(reading.GetControllerButtonState(sample.reading_button_count,
                     sample.reading_button_count == 0U ? nullptr : states.data()),
            sample.reading_button_count);
    for (std::uint32_t index = 0U; index < sample.returned_button_count; ++index) {
        if (states[index]) {
            sample.button_bits |= std::uint64_t{1U} << index;
        }
    }
}

void inspect_contacts(gameinput::IGameInputReading& reading,
    const device_report& device,
    reading_sample& sample) noexcept {
    sample.surface_count = std::min(device.surface_count, max_touch_surfaces);
    sample.surface_count_truncated = device.surface_count_truncated;
    for (std::uint32_t surface = 0U; surface < sample.surface_count; ++surface) {
        const auto contact_count = reading.GetTouchContactCount(surface);
        sample.reported_contact_counts[surface] = contact_count;
        const auto bounded = std::min(contact_count, max_contacts_per_surface);
        sample.contact_count_truncated =
            sample.contact_count_truncated || contact_count > max_contacts_per_surface;
        std::array<gameinput::GameInputTouchContact, max_contacts_per_surface> contacts{};
        const auto returned = std::min(reading.GetTouchSurfaceState(surface,
                                           bounded,
                                           bounded == 0U ? nullptr : contacts.data()),
            bounded);
        sample.returned_contact_counts[surface] = returned;
        for (std::uint32_t index = 0U; index < returned; ++index) {
            sample.contacts[surface][index] =
                reading_contact_payload{.id = contacts[index].contactId,
                    .x = contacts[index].screenX,
                    .y = contacts[index].screenY,
                    .pad_x = contacts[index].padX,
                    .pad_y = contacts[index].padY};
        }
    }
}

void inspect_raw_touch(gameinput::IGameInputReading& reading, reading_sample& sample) noexcept {
    auto& raw_touch = sample.raw_touch;
    raw_touch.attempted = true;
    raw_touch.click_state_available = sample.reading_button_count == dualsense_usb_button_count &&
                                      sample.returned_button_count == dualsense_usb_button_count;
    if (!raw_touch.click_state_available) {
        return;
    }
    raw_touch.click_pressed =
        (sample.button_bits & (std::uint64_t{1U} << dualsense_usb_click_button)) != 0U;
    const auto decoded =
        gameinput_detail::read_dualsense_usb_touch(reading, raw_touch.click_pressed);
    if (!decoded.has_value()) {
        return;
    }
    raw_touch.available = true;
    raw_touch.contact_count = static_cast<std::uint32_t>(decoded->count);
    for (std::size_t index = 0U; index < decoded->count; ++index) {
        const auto& point = decoded->contacts[index];
        raw_touch.contacts[index] = dualsense_usb_touch_point{.id = point.id,
            .x = static_cast<std::int32_t>(point.x),
            .y = static_cast<std::int32_t>(point.y)};
    }
}

[[nodiscard]] input::touch_observation to_resolver_observation(
    const dualsense_usb_touch_record& raw_touch,
    const std::uint64_t timestamp) noexcept {
    input::touch_observation observation{};
    observation.click_status = raw_touch.click_state_available
                                   ? input::observation_status::observed
                                   : input::observation_status::unavailable;
    observation.click_pressed = raw_touch.click_pressed;
    observation.same_reading = raw_touch.click_state_available;
    observation.source_timestamp = timestamp;
    if (!raw_touch.click_state_available) {
        observation.contacts_status = input::observation_status::unavailable;
        return observation;
    }
    if (!raw_touch.click_pressed) {
        observation.contacts_status = input::observation_status::not_applicable;
        return observation;
    }
    if (!raw_touch.available) {
        observation.contacts_status = input::observation_status::unavailable;
        return observation;
    }
    observation.contacts_status = input::observation_status::observed;
    observation.contacts_complete = true;
    observation.surface_width = gameinput_detail::dualsense_usb_touch_width;
    observation.surface_height = gameinput_detail::dualsense_usb_touch_height;
    observation.contact_count = raw_touch.contact_count;
    for (std::size_t index = 0U; index < raw_touch.contact_count; ++index) {
        const auto& contact = raw_touch.contacts[index];
        observation.contacts[index] = input::touch_contact{.id = contact.id,
            .x = static_cast<double>(contact.x),
            .y = static_cast<double>(contact.y),
            .active = true};
    }
    return observation;
}

[[nodiscard]] dualsense_usb_touch_side diagnostic_touch_side(
    const input::touch_source source) noexcept {
    switch (source) {
    case input::touch_source::none:
        return dualsense_usb_touch_side::none;
    case input::touch_source::left:
        return dualsense_usb_touch_side::left;
    case input::touch_source::right:
        return dualsense_usb_touch_side::right;
    case input::touch_source::fallback:
        return dualsense_usb_touch_side::fallback;
    }
    return dualsense_usb_touch_side::none;
}

} // namespace

device_report inspect_device(gameinput::IGameInputDevice& device, const bool raw_usb_requested) {
    device_report report{};
    report.vendor = device.GetVendorId();
    report.product = device.GetProductId();
    report.revision = device.GetRevisionNumber();
    report.supported_input = static_cast<std::uint32_t>(device.GetSupportedInput());
    device.GetHardwareVersion(&report.hardware_version);
    device.GetFirmwareVersion(&report.firmware_version);
    report.display_name =
        read_device_string<512>(device, &gameinput::IGameInputDevice::GetDisplayName);
    report.pnp_path = read_device_string<2048>(device, &gameinput::IGameInputDevice::GetPnpPath);
    report.device_id = format_device_id(device);
    const auto buttons = device.GetControllerButtonCount();
    report.button_count = std::min(buttons, max_button_count);
    report.button_count_truncated = buttons > max_button_count;
    if (report.button_count != 0U) {
        report.labels_returned =
            std::min(device.GetControllerButtonLabels(report.button_count, report.labels.data()),
                report.button_count);
    }
    inspect_touch_surfaces(device, report);
    report.raw_usb_mode_requested = raw_usb_requested;
    if (raw_usb_requested) {
        inspect_raw_report_shape(device, report);
    }
    return report;
}

reading_sample inspect_reading(gameinput::IGameInputReading& reading,
    const device_report& device,
    input::touch_resolver* resolver) noexcept {
    reading_sample sample{};
    sample.timestamp = reading.GetTimestamp();
    sample.input_kind = static_cast<std::uint32_t>(reading.GetInputKind());
    gameinput::GameInputGamepadState gamepad{};
    sample.gamepad_available = reading.GetGamepadState(&gamepad);
    if (sample.gamepad_available) {
        sample.gamepad_buttons = static_cast<std::uint32_t>(gamepad.buttons);
        sample.left_trigger = gamepad.leftTrigger;
        sample.right_trigger = gamepad.rightTrigger;
        sample.left_thumbstick_x = gamepad.leftThumbstickX;
        sample.left_thumbstick_y = gamepad.leftThumbstickY;
        sample.right_thumbstick_x = gamepad.rightThumbstickX;
        sample.right_thumbstick_y = gamepad.rightThumbstickY;
    }
    inspect_buttons(reading, sample);
    if (device.raw_usb_mode_enabled) {
        inspect_raw_touch(reading, sample);
    }
    inspect_contacts(reading, device, sample);
    if (sample.raw_touch.attempted && resolver != nullptr) {
        const auto resolved =
            resolver->observe(to_resolver_observation(sample.raw_touch, sample.timestamp));
        sample.raw_touch.resolved_side = diagnostic_touch_side(resolved.source);
        sample.raw_touch.side_evaluated = true;
    }
    return sample;
}

} // namespace inputnext::input_qualification::diagnostic
