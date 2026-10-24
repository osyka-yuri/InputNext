#pragma once

#include <inputnext/output.hpp>

#include <array>
#include <cstddef>

namespace inputnext::gameinput_detail::dualsense {

using feedback_payload = std::array<std::byte, 47U>;

[[nodiscard]] feedback_payload encode_feedback_payload(
    const output::channel_values& values) noexcept;

} // namespace inputnext::gameinput_detail::dualsense
