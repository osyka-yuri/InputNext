#pragma once

#include "gamepad_state.hpp"

#include <cstdint>

namespace inputnext {

struct input_update final {
    gamepad_state state{};
    std::uint64_t source_timestamp_us{};
    std::uint64_t observation_qpc{};
};

struct published_sample final {
    gamepad_state state{};
    std::uint64_t source_timestamp_us{};
    std::uint64_t observation_qpc{};
    std::uint64_t revision{};

    friend constexpr bool operator==(const published_sample&,
        const published_sample&) noexcept = default;
};

} // namespace inputnext
