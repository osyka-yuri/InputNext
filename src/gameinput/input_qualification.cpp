#include "input_qualification.hpp"
#include "gameinput/input_profile.hpp"

#include <inputnext/device_metadata.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

constexpr input_version qualified_runtime{.major = 4U, .minor = 1U, .build = 314U, .revision = 0U};
constexpr touchpad_surface_shape qualified_touchpad{.width = 1920U,
    .height = 1080U,
    .max_contacts = 2U};

constexpr std::array<std::uint32_t, 15U> dualsense_usb_button_labels{80U,
    78U,
    79U,
    81U,
    112U,
    118U,
    0U,
    0U,
    100U,
    99U,
    114U,
    120U,
    0U,
    0U,
    0U};
constexpr std::uint32_t dualsense_usb_click_button = 13U;
constexpr std::uint32_t maximum_controller_buttons = 64U;

[[nodiscard]] bool matches_transport(const metadata_field<transport_metadata>& transport) noexcept {
    if (!transport.has_value() || transport.value.value != physical_transport::usb) {
        return false;
    }
    return transport.value.provenance == transport_provenance::native_bus_type ||
           transport.value.provenance == transport_provenance::native_enumerator;
}

[[nodiscard]] bool matches_complete_descriptor(const input_qualification_facts& facts) noexcept {
    return facts.button_labels_complete && facts.button_count != 0U &&
           facts.button_count <= maximum_controller_buttons &&
           facts.button_labels.size() == facts.button_count;
}

[[nodiscard]] bool matches_dualsense_usb(const input_qualification_facts& facts) noexcept {
    return facts.runtime_version.has_value() && facts.runtime_version.value == qualified_runtime &&
           facts.hardware_version.has_value() && facts.hardware_version.value == input_version{} &&
           facts.firmware_version.has_value() && facts.firmware_version.value == input_version{} &&
           facts.vendor_id.has_value() && facts.vendor_id.value == 0x054CU &&
           facts.product_id.has_value() && facts.product_id.value == 0x0CE6U &&
           facts.revision.has_value() && facts.revision.value == 256U &&
           facts.supported_input_bits == 22'282'319U && facts.touch_surface_count == 1U &&
           facts.touchpad_surface == qualified_touchpad && matches_transport(facts.transport);
}

[[nodiscard]] bool matches_button_layout(std::span<const std::uint32_t> labels) noexcept {
    if (labels.size() != dualsense_usb_button_labels.size()) {
        return false;
    }
    for (std::uint32_t index = 0U; index < labels.size(); ++index) {
        if (index == dualsense_usb_click_button &&
            labels[index] == gameinput_touchpad_button_label) {
            continue;
        }
        if (labels[index] != dualsense_usb_button_labels[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

native_input_profile qualify_native_input(const input_qualification_facts& facts) noexcept {
    native_input_profile profile{};
    if (facts.runtime_version.has_value()) {
        const auto& version = facts.runtime_version.value;
        profile.runtime_version = {version.major, version.minor, version.build, version.revision};
    }
    if (facts.transport.has_value()) {
        profile.transport = facts.transport.value.value;
    }
    if (!facts.gamepad_supported || !facts.controller_button_supported) {
        profile.reason = input_fallback_reason::unsupported_capability;
        return profile;
    }
    if (!matches_complete_descriptor(facts)) {
        profile.reason = input_fallback_reason::incoherent_reading;
        return profile;
    }
    if (!matches_dualsense_usb(facts) || !matches_button_layout(facts.button_labels)) {
        return profile;
    }

    profile.history = input_history_path::composite;
    profile.reason = input_fallback_reason::none;
    profile.callback_coherent = true;
    profile.coordinates = touch_coordinate_source::surface;
    profile.button_count = facts.button_count;
    profile.click_button = dualsense_usb_click_button;
    profile.surface_width = qualified_touchpad.width;
    profile.surface_height = qualified_touchpad.height;
    return profile;
}

} // namespace inputnext::gameinput_detail
