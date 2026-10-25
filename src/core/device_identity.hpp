#pragma once

#include "core.hpp"
#include <inputnext/device_metadata.hpp>

#include <array>
#include <cstdint>

namespace inputnext::core::detail {

struct projected_identity final {
    identity_quality quality{identity_quality::none};
    std::array<std::uint8_t, 32U> fingerprint{};
};

[[nodiscard]] projected_identity fingerprint_device(const metadata_snapshot& metadata);

} // namespace inputnext::core::detail
