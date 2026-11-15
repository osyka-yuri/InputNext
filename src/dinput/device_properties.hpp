#pragma once

#include "native_types.hpp"
#include "profile.hpp"

#include <array>
#include <cstdint>

namespace inputnext::dinput {

struct compiled_format;
struct device_identity;

using axis_configuration = std::array<axis_properties, axis_count>;

struct property_change final {
    enum class setting : std::uint8_t { range, deadzone, saturation, axis_mode, buffer_size };

    setting kind{setting::range};
    LONG minimum{};
    LONG maximum{};
    DWORD value{};

    [[nodiscard]] bool affects_axes() const noexcept {
        return kind != setting::axis_mode && kind != setting::buffer_size;
    }
};

[[nodiscard]] bool valid_property_header(const DIPROPHEADER& header) noexcept;
[[nodiscard]] HRESULT read_device_property(const GUID* token,
    DIPROPHEADER* header,
    const device_identity& identity,
    const compiled_format* format,
    const axis_configuration& axes) noexcept;

// Decode the request before acquisition gating. Apply it to a candidate under
// the device's gate, then let the device publish it with a new generation.
[[nodiscard]] HRESULT decode_property_change(const GUID* token,
    const DIPROPHEADER& header,
    property_change& change) noexcept;
[[nodiscard]] HRESULT apply_property_change(const property_change& change,
    const DIPROPHEADER& header,
    const compiled_format* format,
    axis_configuration& candidate) noexcept;

} // namespace inputnext::dinput
