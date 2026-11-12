#include "input_qualification.hpp"
#include "gameinput/dualsense/usb_touch_report.hpp"
#include "gameinput/input_profile.hpp"

#include <inputnext/device_metadata.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

struct qualification_policy_row final {
    input_version runtime{};
    input_version hardware{};
    input_version firmware{};
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t revision{};
    std::uint32_t supported_input_bits{};
    bool gamepad_supported{};
    bool controller_button_supported{};
    std::uint32_t button_count{};
    std::array<std::uint32_t, 15U> labels{};
    std::uint32_t touch_surface_count{};
    input_history_path history{};
};

constexpr qualification_policy_row dualsense_usb_runtime_4_0_306_0{
    .runtime = {.major = 4U, .minor = 0U, .build = 306U, .revision = 0U},
    .hardware = {.major = 0U, .minor = 0U, .build = 0U, .revision = 0U},
    .firmware = {.major = 0U, .minor = 0U, .build = 0U, .revision = 0U},
    .vendor_id = 0x054CU,
    .product_id = 0x0CE6U,
    .revision = 100U,
    .supported_input_bits = 18'088'015U,
    .gamepad_supported = true,
    .controller_button_supported = true,
    .button_count = 15U,
    .labels = {80U, 78U, 79U, 81U, 112U, 118U, 0U, 0U, 100U, 99U, 114U, 120U, 0U, 0U, 0U},
    .touch_surface_count = 0U,
    .history = input_history_path::composite,
};

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

[[nodiscard]] bool matches_policy(const input_qualification_facts& facts,
    const qualification_policy_row& policy) noexcept {
    if (!facts.runtime_version.has_value() || !facts.hardware_version.has_value() ||
        !facts.firmware_version.has_value() || !facts.vendor_id.has_value() ||
        !facts.product_id.has_value() || !facts.revision.has_value()) {
        return false;
    }

    const auto& expected = policy;
    if (facts.runtime_version.value != expected.runtime ||
        facts.hardware_version.value != expected.hardware ||
        facts.firmware_version.value != expected.firmware ||
        facts.vendor_id.value != expected.vendor_id ||
        facts.product_id.value != expected.product_id ||
        facts.revision.value != expected.revision ||
        facts.supported_input_bits != expected.supported_input_bits ||
        facts.gamepad_supported != expected.gamepad_supported ||
        facts.controller_button_supported != expected.controller_button_supported ||
        facts.button_count != expected.button_count ||
        facts.touch_surface_count != expected.touch_surface_count ||
        !matches_transport(facts.transport)) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.labels.size(); ++index) {
        if (facts.button_labels[index] != expected.labels[index]) {
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
    if (!matches_policy(facts, dualsense_usb_runtime_4_0_306_0)) {
        profile.reason = input_fallback_reason::unsupported_capability;
        return profile;
    }

    profile.history = dualsense_usb_runtime_4_0_306_0.history;
    profile.reason = input_fallback_reason::none;
    profile.callback_coherent = true;
    profile.button_count = dualsense_usb_runtime_4_0_306_0.button_count;
    profile.click_button = 13U;
    // The supervised USB captures verified this report shape in both observation streams.
    if (facts.single_input_report.has_value() &&
        facts.single_input_report->id == dualsense_usb_input_report_id &&
        facts.single_input_report->size == dualsense_usb_input_report_size) {
        profile.coordinates = touch_coordinate_source::dualsense_usb_report;
    }
    return profile;
}

} // namespace inputnext::gameinput_detail
