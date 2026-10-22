#include <gameinput/output/connections/schedule.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;
using connection_schedule::creation_state;
using connection_schedule::description_due;
using connection_schedule::include_creation_deadline;
using connection_schedule::retry_state;
using connection_schedule::time_point;

TEST_CASE("gameinput.output.connection_schedule_retries_after_latest_active_gate",
    "[gameinput][output][unit][retry][scheduling]") {
    const time_point origin{100ms};
    const creation_state eligible{.accepting = true,
        .fallback = false,
        .admissions_frozen = false,
        .occupied = true,
        .description_known = true,
        .has_rumble_capability = true,
        .retry_blocked = false};

    SECTION("both deadlines are future") {
        std::optional<time_point> nearest;
        const retry_state gates{.retry_armed = true,
            .retry_due = origin + 8ms,
            .resource_waiting = true,
            .resource_due = origin + 20ms};
        include_creation_deadline(nearest, eligible, gates);
        CHECK(nearest == std::optional{origin + 20ms});
    }

    SECTION("an expired retry waits for future resource capacity") {
        std::optional<time_point> nearest;
        const retry_state gates{.retry_armed = true,
            .retry_due = origin - 8ms,
            .resource_waiting = true,
            .resource_due = origin + 20ms};
        include_creation_deadline(nearest, eligible, gates);
        CHECK(nearest == std::optional{origin + 20ms});
    }

    SECTION("ready resource capacity waits for a future retry") {
        std::optional<time_point> nearest;
        const retry_state gates{.retry_armed = true,
            .retry_due = origin + 20ms,
            .resource_waiting = true,
            .resource_due = origin - 8ms};
        include_creation_deadline(nearest, eligible, gates);
        CHECK(nearest == std::optional{origin + 20ms});
    }

    SECTION("unknown, unsupported, and blocked entries leave other deadlines unchanged") {
        const retry_state expired_retry{.retry_armed = true,
            .retry_due = origin - 8ms,
            .resource_waiting = false,
            .resource_due = origin};
        const auto backend_progress_due = origin + 4ms;
        const auto description_due_time = origin + 64ms;
        const std::array ineligible{creation_state{.accepting = true,
                                        .fallback = false,
                                        .admissions_frozen = false,
                                        .occupied = true,
                                        .description_known = false,
                                        .has_rumble_capability = true,
                                        .retry_blocked = false},
            creation_state{.accepting = true,
                .fallback = false,
                .admissions_frozen = false,
                .occupied = true,
                .description_known = true,
                .has_rumble_capability = false,
                .retry_blocked = false},
            creation_state{.accepting = true,
                .fallback = false,
                .admissions_frozen = false,
                .occupied = true,
                .description_known = true,
                .has_rumble_capability = true,
                .retry_blocked = true}};
        for (const auto gates : ineligible) {
            auto nearest = std::optional{description_due_time};
            if (backend_progress_due < description_due_time) {
                nearest = backend_progress_due;
            }
            include_creation_deadline(nearest, gates, expired_retry);
            CHECK(nearest == std::optional{backend_progress_due});
        }
    }
}

TEST_CASE("gameinput.output.connection_schedule_checks_description_deadline_for_every_target",
    "[gameinput][output][unit][description][scheduling]") {
    const time_point origin{100ms};

    SECTION("a fresh occupied entry is immediately eligible") {
        CHECK(description_due(true, origin, time_point::min()));
    }

    SECTION("unknown entries with a future deadline are skipped on every four millisecond scan") {
        const auto due = origin + 64ms;
        for (auto scan = origin; scan < due; scan += 4ms) {
            CHECK((!description_due(true, scan, due)));
        }
        CHECK(description_due(true, due, due));
    }

    SECTION("known entries use the same completion relative deadline boundary") {
        const auto due = origin + 64ms;
        CHECK((!description_due(true, due - 4ms, due)));
        CHECK(description_due(true, due, due));
    }

    SECTION("unoccupied entries are never description eligible") {
        CHECK((!description_due(false, origin + 64ms, origin + 64ms)));
    }
}

} // namespace
} // namespace inputnext::gameinput_detail
