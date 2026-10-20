#include "query_projection.hpp"

#include <inputnext/device_metadata.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace inputnext::xinput::detail {
namespace {

void initialize_gamepad_capabilities(XINPUT_CAPABILITIES& capabilities) noexcept {
    capabilities.Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities.Flags = 0U;
    capabilities.Gamepad.wButtons = 0xF3FFU;
    capabilities.Gamepad.bLeftTrigger = 255U;
    capabilities.Gamepad.bRightTrigger = 255U;
    capabilities.Gamepad.sThumbLX = static_cast<SHORT>(-32768);
    capabilities.Gamepad.sThumbLY = static_cast<SHORT>(-32768);
    capabilities.Gamepad.sThumbRX = static_cast<SHORT>(-32768);
    capabilities.Gamepad.sThumbRY = static_cast<SHORT>(-32768);
}

void apply_dynamic_capabilities(XINPUT_CAPABILITIES& capabilities,
    const metadata_snapshot& metadata) noexcept {
    if (metadata.transport.has_value() &&
        (metadata.transport.value.value == physical_transport::bluetooth ||
            metadata.transport.value.value == physical_transport::other_wireless)) {
        capabilities.Flags = static_cast<WORD>(capabilities.Flags | XINPUT_CAPS_WIRELESS);
    }
    if (metadata.capabilities.has_value()) {
        const bool low_frequency = metadata.capabilities.value.low_frequency_rumble;
        const bool high_frequency = metadata.capabilities.value.high_frequency_rumble;
        if (low_frequency || high_frequency) {
            capabilities.Flags = static_cast<WORD>(capabilities.Flags | XINPUT_CAPS_FFB_SUPPORTED);
        }
        capabilities.Vibration.wLeftMotorSpeed = low_frequency ? 65535U : 0U;
        capabilities.Vibration.wRightMotorSpeed = high_frequency ? 65535U : 0U;
    }
    if (has_voice_association(metadata)) {
        capabilities.Flags = static_cast<WORD>(capabilities.Flags | XINPUT_CAPS_VOICE_SUPPORTED);
    }
}

} // namespace

XINPUT_CAPABILITIES base_capabilities() noexcept {
    XINPUT_CAPABILITIES capabilities{};
    initialize_gamepad_capabilities(capabilities);
    return capabilities;
}

XINPUT_CAPABILITIES dynamic_capabilities(const metadata_snapshot& metadata) noexcept {
    auto capabilities = base_capabilities();
    apply_dynamic_capabilities(capabilities, metadata);
    return capabilities;
}

capabilities_ex extended_capabilities(const metadata_snapshot& metadata) noexcept {
    capabilities_ex result{};
    result.capabilities = dynamic_capabilities(metadata);
    result.vendor_id = metadata.native.vendor_id.has_value() ? metadata.native.vendor_id.value : 0U;
    result.product_id =
        metadata.native.product_id.has_value() ? metadata.native.product_id.value : 0U;
    result.product_version =
        metadata.native.revision.has_value() ? metadata.native.revision.value : 0U;
    result.unknown1 = 0U;
    return result;
}

XINPUT_BATTERY_INFORMATION gamepad_battery(const metadata_snapshot& metadata) noexcept {
    XINPUT_BATTERY_INFORMATION result{};
    result.BatteryType =
        metadata.transport.has_value() && metadata.transport.value.value == physical_transport::usb
            ? BATTERY_TYPE_WIRED
            : BATTERY_TYPE_UNKNOWN;
    result.BatteryLevel = BATTERY_LEVEL_EMPTY;
    return result;
}

bool has_voice_association(const metadata_snapshot& metadata) noexcept {
    const auto has_endpoint = [](const audio_endpoint_metadata& endpoint) noexcept {
        return endpoint.id.has_value() && !endpoint.id.value.empty() &&
               endpoint.id.value.size() <= headset_endpoint_content_code_units;
    };
    const auto has_guid = [](const audio_endpoint_metadata& endpoint) noexcept {
        return endpoint.direct_sound_guid.has_value() &&
               !endpoint.direct_sound_guid.value.is_zero();
    };
    return has_endpoint(metadata.headset_render) || has_endpoint(metadata.headset_capture) ||
           has_guid(metadata.headset_render) || has_guid(metadata.headset_capture);
}

std::u16string_view endpoint_id(const audio_endpoint_metadata& endpoint) noexcept {
    if (!endpoint.id.has_value() ||
        endpoint.id.value.size() > headset_endpoint_content_code_units) {
        return {};
    }
    return endpoint.id.value;
}

endpoint_write_result write_endpoint(std::u16string_view endpoint,
    LPWSTR buffer,
    UINT* count,
    std::uint32_t count_written_bit,
    std::uint32_t buffer_written_bit,
    std::uint32_t* write_flags) noexcept {
    if (count == nullptr) {
        return endpoint_write_result::success;
    }

    if (endpoint.empty()) {
        if (buffer != nullptr && *count > 0U) {
            buffer[0] = L'\0';
            if (write_flags != nullptr) {
                *write_flags |= buffer_written_bit;
            }
        }
        *count = 0U;
        if (write_flags != nullptr) {
            *write_flags |= count_written_bit;
        }
        return endpoint_write_result::success;
    }

    const auto required_count = static_cast<UINT>(endpoint.size() + 1U);
    if (buffer == nullptr) {
        *count = required_count;
        if (write_flags != nullptr) {
            *write_flags |= count_written_bit;
        }
        return endpoint_write_result::success;
    }
    if (*count < required_count) {
        *count = required_count;
        if (write_flags != nullptr) {
            *write_flags |= count_written_bit;
        }
        return endpoint_write_result::insufficient_buffer;
    }

    for (std::size_t index = 0U; index < endpoint.size(); ++index) {
        buffer[index] = static_cast<wchar_t>(endpoint[index]);
    }
    buffer[endpoint.size()] = L'\0';
    *count = required_count;
    if (write_flags != nullptr) {
        *write_flags |= count_written_bit | buffer_written_bit;
    }
    return endpoint_write_result::success;
}

GUID project_guid(const metadata_field<native_guid>& field) noexcept {
    GUID result_guid{};
    if (field.has_value()) {
        static_assert(sizeof(result_guid) == native_guid_bytes);
        std::memcpy(&result_guid, field.value.bytes.data(), native_guid_bytes);
    }
    return result_guid;
}

} // namespace inputnext::xinput::detail
