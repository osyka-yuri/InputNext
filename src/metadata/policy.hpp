#pragma once

#include <inputnext/device_metadata.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace inputnext::metadata_detail {

struct utf16_buffer_view final {
    const char16_t* data{};
    std::size_t code_units{};
};

[[nodiscard]] metadata_field<std::string> copy_bounded_pnp_path(std::string_view input,
    std::uint32_t native_code = 0U);
[[nodiscard]] metadata_field<std::u16string> copy_bounded_utf16(utf16_buffer_view input,
    std::size_t max_content_code_units);
[[nodiscard]] metadata_field<std::u16string> copy_bounded_utf16_content(std::u16string_view input,
    std::size_t max_content_code_units);
[[nodiscard]] metadata_field<transport_metadata> infer_transport_from_pnp_path(
    std::string_view pnp_path);
[[nodiscard]] metadata_field<metadata_device_model> classify_device_model(
    const native_device_facts& facts) noexcept;
[[nodiscard]] metadata_field<hardware_capabilities> derive_hardware_capabilities(
    const native_device_facts& facts) noexcept;

} // namespace inputnext::metadata_detail
