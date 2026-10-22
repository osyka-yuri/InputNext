#include "drive_policy.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace inputnext::output::detail::drive_policy {

std::uint32_t next_retryable_failure_count(const std::uint32_t current) noexcept {
    if (current == std::numeric_limits<std::uint32_t>::max()) {
        return current;
    }
    return current + 1U;
}

std::chrono::milliseconds retry_delay_for(const std::uint32_t failures) noexcept {
    if (failures == 0U) {
        return std::chrono::milliseconds::zero();
    }

    constexpr std::array delays{retry_base_delay,
        retry_base_delay * 2,
        retry_base_delay * 4,
        retry_base_delay * 8,
        retry_maximum_delay};
    const auto index = static_cast<std::size_t>(failures - 1U);
    return index < delays.size() ? delays[index] : delays.back();
}

std::chrono::steady_clock::time_point deadline_after(
    const std::chrono::steady_clock::time_point now,
    const std::chrono::steady_clock::duration delay) noexcept {
    using clock = std::chrono::steady_clock;
    if (delay <= clock::duration::zero()) {
        return now;
    }
    const auto now_count = now.time_since_epoch().count();
    const auto delay_count = delay.count();
    const auto maximum_count = clock::duration::max().count();
    if (now_count >= 0 && delay_count > maximum_count - now_count) {
        return clock::time_point::max();
    }
    return now + delay;
}

} // namespace inputnext::output::detail::drive_policy
