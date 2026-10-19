#include "clock_mapping.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace inputnext::gameinput_detail {
namespace {

constexpr std::uint64_t microseconds_per_second = 1'000'000U;

[[nodiscard]] std::optional<std::uint64_t> map_unclamped(const history_clock_anchor& anchor,
    std::uint64_t source_timestamp_us) noexcept {
    if (anchor.qpc == 0U || anchor.qpc_frequency == 0U) {
        return std::nullopt;
    }

    const bool timestamp_is_after_anchor = source_timestamp_us >= anchor.source_timestamp_us;
    const auto delta_us = timestamp_is_after_anchor
                              ? source_timestamp_us - anchor.source_timestamp_us
                              : anchor.source_timestamp_us - source_timestamp_us;
    const auto whole_seconds = delta_us / microseconds_per_second;
    const auto fractional_microseconds = delta_us % microseconds_per_second;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

    if (whole_seconds != 0U && anchor.qpc_frequency > maximum / whole_seconds) {
        return std::nullopt;
    }
    if (fractional_microseconds != 0U && anchor.qpc_frequency > maximum / fractional_microseconds) {
        return std::nullopt;
    }

    const auto whole_ticks = whole_seconds * anchor.qpc_frequency;
    const auto fractional_ticks =
        (fractional_microseconds * anchor.qpc_frequency) / microseconds_per_second;
    if (fractional_ticks > maximum - whole_ticks) {
        return std::nullopt;
    }

    const auto offset = whole_ticks + fractional_ticks;
    if (timestamp_is_after_anchor) {
        if (offset > maximum - anchor.qpc) {
            return std::nullopt;
        }
        return anchor.qpc + offset;
    }

    if (offset >= anchor.qpc) {
        return std::nullopt;
    }
    return anchor.qpc - offset;
}

} // namespace

std::optional<mapped_history_time> map_history_timestamp(const history_clock_anchor& anchor,
    std::uint64_t source_timestamp_us,
    std::optional<std::uint64_t> qpc_floor) noexcept {
    const auto mapped = map_unclamped(anchor, source_timestamp_us);
    if (!mapped.has_value()) {
        return std::nullopt;
    }

    if (qpc_floor.has_value() && *mapped < *qpc_floor) {
        if (*qpc_floor == 0U) {
            return std::nullopt;
        }
        return mapped_history_time{.qpc = *qpc_floor, .adjusted = true};
    }
    return mapped_history_time{.qpc = *mapped, .adjusted = false};
}

} // namespace inputnext::gameinput_detail
