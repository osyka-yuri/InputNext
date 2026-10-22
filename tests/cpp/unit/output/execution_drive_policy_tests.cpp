#include "output/execution/drive_policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using clock = std::chrono::steady_clock;
using inputnext::output::detail::drive_policy::deadline_after;
using inputnext::output::detail::drive_policy::next_retryable_failure_count;
using inputnext::output::detail::drive_policy::retry_delay_for;

TEST_CASE("output.drive_policy_retry_delay_grows_to_a_bounded_cap", "[unit][output][execution]") {
    constexpr std::array<std::chrono::milliseconds, 8U> expected{
        std::chrono::milliseconds{0},
        std::chrono::milliseconds{4},
        std::chrono::milliseconds{8},
        std::chrono::milliseconds{16},
        std::chrono::milliseconds{32},
        std::chrono::milliseconds{64},
        std::chrono::milliseconds{64},
        std::chrono::milliseconds{64},
    };

    for (std::size_t index = 0U; index < expected.size(); ++index) {
        CHECK(retry_delay_for(static_cast<std::uint32_t>(index)) == expected[index]);
    }
    CHECK(retry_delay_for(std::numeric_limits<std::uint32_t>::max()) ==
          std::chrono::milliseconds{64});
}

TEST_CASE("output.drive_policy_failure_count_saturates_without_wrapping",
    "[unit][output][execution]") {
    CHECK(next_retryable_failure_count(0U) == 1U);
    CHECK(next_retryable_failure_count(4U) == 5U);
    CHECK(next_retryable_failure_count(std::numeric_limits<std::uint32_t>::max()) ==
          std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("output.drive_policy_deadline_saturates_at_clock_max", "[unit][output][execution]") {
    constexpr auto start = clock::time_point{clock::duration{10}};
    CHECK(deadline_after(start, clock::duration{3}) == clock::time_point{clock::duration{13}});
    CHECK(deadline_after(start, clock::duration::zero()) == start);
    CHECK(deadline_after(start, clock::duration{-1}) == start);
    CHECK(deadline_after(clock::time_point::max() - clock::duration{1}, clock::duration{2}) ==
          clock::time_point::max());
    CHECK(deadline_after(clock::time_point::min(), clock::duration::max()) ==
          clock::time_point{clock::duration{-1}});
}

} // namespace
