#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace inputnext::gameinput_detail::connection_schedule {

using time_point = std::chrono::steady_clock::time_point;

[[nodiscard]] constexpr bool
description_due(const bool occupied, const time_point now, const time_point due) noexcept {
    return occupied && now >= due;
}

struct creation_state final {
    bool accepting{};
    bool fallback{};
    bool admissions_frozen{};
    bool occupied{};
    bool description_known{};
    bool has_rumble_capability{};
    bool retry_blocked{};
};

struct retry_state final {
    bool retry_armed{};
    time_point retry_due;
    bool resource_waiting{};
    time_point resource_due;
};

[[nodiscard]] constexpr bool can_attempt_creation(const creation_state state) noexcept {
    return state.accepting && !state.fallback && !state.admissions_frozen && state.occupied &&
           state.description_known && state.has_rumble_capability && !state.retry_blocked;
}

inline void include_creation_deadline(std::optional<time_point>& nearest,
    const creation_state state,
    const retry_state retry) noexcept {
    if (!can_attempt_creation(state)) {
        return;
    }

    std::optional<time_point> due;
    if (retry.retry_armed && retry.resource_waiting) {
        due = std::max(retry.retry_due, retry.resource_due);
    } else if (retry.retry_armed) {
        due = retry.retry_due;
    } else if (retry.resource_waiting) {
        due = retry.resource_due;
    }

    if (!due.has_value()) {
        return;
    }
    const auto candidate = due.value_or(time_point::max());
    if (candidate < nearest.value_or(time_point::max())) {
        nearest = candidate;
    }
}

} // namespace inputnext::gameinput_detail::connection_schedule
