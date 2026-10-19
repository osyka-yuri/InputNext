#pragma once

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

struct history_clock_anchor final {
    std::uint64_t source_timestamp_us{};
    std::uint64_t qpc{};
    std::uint64_t qpc_frequency{};
};

struct mapped_history_time final {
    std::uint64_t qpc{};
    bool adjusted{};

    friend constexpr bool operator==(const mapped_history_time&,
        const mapped_history_time&) noexcept = default;
};

[[nodiscard]] std::optional<mapped_history_time> map_history_timestamp(
    const history_clock_anchor& anchor,
    std::uint64_t source_timestamp_us,
    std::optional<std::uint64_t> qpc_floor = std::nullopt) noexcept;

} // namespace inputnext::gameinput_detail
