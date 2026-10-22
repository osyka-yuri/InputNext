#pragma once

#include <chrono>
#include <cstdint>

namespace inputnext::output::detail::drive_policy {

inline constexpr std::chrono::milliseconds retry_base_delay{4};
inline constexpr std::chrono::milliseconds retry_maximum_delay{64};
inline constexpr std::chrono::milliseconds refresh_interval{4};

[[nodiscard]] std::uint32_t next_retryable_failure_count(std::uint32_t current) noexcept;

[[nodiscard]] std::chrono::milliseconds retry_delay_for(std::uint32_t failures) noexcept;

[[nodiscard]] std::chrono::steady_clock::time_point deadline_after(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration delay) noexcept;

} // namespace inputnext::output::detail::drive_policy
