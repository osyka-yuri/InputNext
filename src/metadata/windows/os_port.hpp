#pragma once

#include <inputnext/device_metadata.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace inputnext::metadata_detail {

enum class audio_flow : std::uint8_t {
    render,
    capture,
};

struct exact_pnp_observation final {
    metadata_field<native_guid> container_id{};
    metadata_field<transport_metadata> transport{};
};

struct audio_endpoint_observation final {
    metadata_field<native_guid> container_id{};
    metadata_field<std::u16string> endpoint_id{};
    metadata_field<native_guid> direct_sound_guid{};
};

struct audio_scan_result final {
    metadata_availability availability{metadata_availability::unavailable};
    std::uint32_t native_code{};
};

using audio_endpoint_visitor = void (*)(void*, const audio_endpoint_observation&);

// Narrow caller-side OS seam: one exact PnP interface and a streaming active-endpoint scan per flow.
class windows_metadata_os {
public:
    windows_metadata_os(const windows_metadata_os&) = delete;
    windows_metadata_os& operator=(const windows_metadata_os&) = delete;
    windows_metadata_os(windows_metadata_os&&) = delete;
    windows_metadata_os& operator=(windows_metadata_os&&) = delete;
    virtual ~windows_metadata_os() = default;
    [[nodiscard]] virtual exact_pnp_observation inspect_exact_pnp_interface(
        std::string_view pnp_path_utf8) = 0;
    [[nodiscard]] virtual audio_scan_result scan_active_audio(audio_flow flow,
        const native_guid& container_id,
        audio_endpoint_visitor visitor,
        void* visitor_context) = 0;

protected:
    windows_metadata_os() = default;
};

[[nodiscard]] std::unique_ptr<windows_metadata_os> make_system_windows_metadata_os();

} // namespace inputnext::metadata_detail
