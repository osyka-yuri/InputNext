#pragma once

#include <inputnext/gamepad_state.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

struct reading_parts final {
    std::optional<gamepad_state> pad = std::nullopt;
    std::optional<bool> guide = std::nullopt;
    std::uint64_t source_timestamp_us{};
    std::uint64_t observation_qpc{};
};

} // namespace inputnext::gameinput_detail
