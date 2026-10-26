#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <array>
#include <cstddef>

namespace inputnext::gameinput_detail::dualsense {

using feedback_payload = std::array<std::byte, 47U>;

[[nodiscard]] feedback_payload encode_feedback_payload(const output::channel_values& values,
    output::body_actuator_route route = output::body_actuator_route::classic) noexcept;

} // namespace inputnext::gameinput_detail::dualsense
