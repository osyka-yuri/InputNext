#include <gameinput/history/clock_mapping.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE("gameinput.history.clock_maps_fractional_microseconds", "[unit][gameinput][clock]") {
    constexpr history_clock_anchor anchor{
        .source_timestamp_us = 1'500'000U,
        .qpc = 42'000'000U,
        .qpc_frequency = 12'500'000U,
    };

    const auto forward = map_history_timestamp(anchor, 1'500'001U);
    const auto backward = map_history_timestamp(anchor, 1'499'999U);
    constexpr mapped_history_time expected_forward{.qpc = 42'000'012U, .adjusted = false};
    constexpr mapped_history_time expected_backward{.qpc = 41'999'988U, .adjusted = false};

    REQUIRE(forward == expected_forward);
    REQUIRE(backward == expected_backward);
}

TEST_CASE("gameinput.history.clock_preserves_equal_timestamps", "[unit][gameinput][clock]") {
    constexpr history_clock_anchor anchor{
        .source_timestamp_us = 4'000U,
        .qpc = 7'000U,
        .qpc_frequency = 10'000'000U,
    };

    const auto first = map_history_timestamp(anchor, 4'001U);
    const auto second = map_history_timestamp(anchor, 4'001U);
    const auto clamped = map_history_timestamp(anchor, 4'001U, 7'100U);
    constexpr mapped_history_time expected_clamp{.qpc = 7'100U, .adjusted = true};
    constexpr mapped_history_time expected_mapping{.qpc = 7'010U, .adjusted = false};

    REQUIRE(first == expected_mapping);
    REQUIRE(second == expected_mapping);
    REQUIRE(clamped == expected_clamp);
}

TEST_CASE("gameinput.history.clock_rejects_unrepresentable_time", "[unit][gameinput][clock]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

    REQUIRE_FALSE(map_history_timestamp(
        history_clock_anchor{
            .source_timestamp_us = 0U,
            .qpc = 1U,
            .qpc_frequency = maximum,
        },
        2U)
            .has_value());
    REQUIRE_FALSE(map_history_timestamp(
        history_clock_anchor{
            .source_timestamp_us = maximum,
            .qpc = 1U,
            .qpc_frequency = 1U,
        },
        0U)
            .has_value());
}

TEST_CASE("gameinput.history.clock_requires_positive_anchor_and_frequency",
    "[unit][gameinput][clock]") {
    REQUIRE_FALSE(map_history_timestamp(
        history_clock_anchor{
            .source_timestamp_us = 0U,
            .qpc = 0U,
            .qpc_frequency = 1U,
        },
        0U)
            .has_value());
    REQUIRE_FALSE(map_history_timestamp(
        history_clock_anchor{
            .source_timestamp_us = 0U,
            .qpc = 1U,
            .qpc_frequency = 0U,
        },
        0U)
            .has_value());
}

} // namespace
} // namespace inputnext::gameinput_detail
