#pragma once

#include "gameinput/runtime/output_access.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/output.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

enum class output_backend_kind : std::uint8_t {
    none,
    gameinput_rumble,
    dualsense_usb,
};

[[nodiscard]] constexpr output::channel_mask represented_channels(
    const output_backend_kind kind) noexcept {
    switch (kind) {
    case output_backend_kind::none:
        return 0U;
    case output_backend_kind::gameinput_rumble:
        return output::rumble_mask;
    case output_backend_kind::dualsense_usb:
        return output::supported_channel_mask;
    }
    return 0U;
}

[[nodiscard]] constexpr bool needs_usb_qualification(
    const std::optional<metadata_device_model> model) noexcept {
    return model == metadata_device_model::dualsense ||
           model == metadata_device_model::dualsense_edge;
}

[[nodiscard]] constexpr output_backend_kind fallback_backend(
    const std::optional<rumble_capabilities>& capabilities) noexcept {
    return capabilities.has_value() && (capabilities->low_frequency || capabilities->high_frequency)
               ? output_backend_kind::gameinput_rumble
               : output_backend_kind::none;
}

} // namespace inputnext::gameinput_detail
