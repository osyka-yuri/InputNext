#pragma once

#include <inputnext/device_metadata.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <string_view>

namespace inputnext::xinput::detail {

enum class endpoint_write_result : std::uint8_t {
    success,
    insufficient_buffer,
};

[[nodiscard]] XINPUT_CAPABILITIES base_capabilities() noexcept;
[[nodiscard]] XINPUT_CAPABILITIES dynamic_capabilities(const metadata_snapshot& metadata) noexcept;
[[nodiscard]] capabilities_ex extended_capabilities(const metadata_snapshot& metadata) noexcept;
[[nodiscard]] XINPUT_BATTERY_INFORMATION gamepad_battery(
    const metadata_snapshot& metadata) noexcept;
[[nodiscard]] bool has_voice_association(const metadata_snapshot& metadata) noexcept;
[[nodiscard]] std::u16string_view endpoint_id(const audio_endpoint_metadata& endpoint) noexcept;
[[nodiscard]] endpoint_write_result write_endpoint(std::u16string_view endpoint,
    LPWSTR buffer,
    UINT* count,
    std::uint32_t count_written_bit,
    std::uint32_t buffer_written_bit,
    std::uint32_t* write_flags) noexcept;
[[nodiscard]] GUID project_guid(const metadata_field<native_guid>& field) noexcept;

} // namespace inputnext::xinput::detail
