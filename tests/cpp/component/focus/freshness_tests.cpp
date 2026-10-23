#include <inputnext/focus.hpp>

#include "focus/observer_test_access.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::focus;
using namespace inputnext::test::focus;

[[nodiscard]] runtime_id make_incarnation(std::byte first_byte) noexcept {
    runtime_id value{};
    value.bytes[0U] = first_byte;
    return value;
}

template <typename Predicate>
[[nodiscard]] bool wait_for_publication(const observer& value, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        if (predicate(value.published())) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class source_release final {
public:
    explicit source_release(controlled_source& source) noexcept : source_(source) {}

    ~source_release() { source_.release(); }

    source_release(const source_release&) = delete;
    source_release& operator=(const source_release&) = delete;
    source_release(source_release&&) = delete;
    source_release& operator=(source_release&&) = delete;

private:
    controlled_source& source_;
};

[[nodiscard]] std::unique_ptr<observer>
make_observer(manual_clock& clock, runtime_id incarnation, std::unique_ptr<source> owned_source) {
    return detail::observer_test_access::make_with_clock(incarnation,
        std::move(owned_source),
        &manual_clock::read,
        &clock);
}

TEST_CASE("focus.observer_foreground_freshness_expires_at_strict_budget",
    "[component][focus][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 71U, .creation_time_100ns = 1001U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{71U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    const auto captured = value->published();

    {
        const auto guard = value->lock_presentation();
        clock.advance(249ms);
        CHECK(guard.matches_foreground(captured));
        if (const auto before_capture = guard.capture_foreground(); before_capture.has_value()) {
            CHECK(before_capture->revision == captured.revision);
        } else {
            FAIL("fresh foreground evidence is missing before strict expiry");
        }
        const auto before_expiry = guard.published();
        CHECK(before_expiry.state == evidence::foreground);
        CHECK(before_expiry.revision == captured.revision);

        clock.advance(1ms);
        CHECK(!guard.matches_foreground(captured));
        CHECK(!guard.capture_foreground().has_value());
        const auto at_expiry = guard.published();
        CHECK(at_expiry.state == evidence::unavailable);
        CHECK(at_expiry.revision == captured.revision + 1U);
        CHECK(at_expiry.observation_started == std::chrono::steady_clock::time_point{});

        clock.advance(1ms);
        const auto after_expiry = guard.published();
        CHECK(after_expiry.state == evidence::unavailable);
        CHECK(after_expiry.revision == at_expiry.revision);
    }

    source->release();
    value->stop();
}

TEST_CASE("focus.presentation_guard_capture_foreground_expires_at_strict_budget",
    "[component][focus][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 82U, .creation_time_100ns = 1011U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{82U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());

    {
        const auto guard = value->lock_presentation();
        clock.advance(250ms);
        CHECK(!guard.capture_foreground().has_value());

        const auto expired = guard.published();
        CHECK(expired.state == evidence::unavailable);
        CHECK(expired.revision == 2U);
        CHECK(expired.observation_started == std::chrono::steady_clock::time_point{});
        CHECK(!guard.capture_foreground().has_value());
    }

    source->release();
    value->stop();
}

TEST_CASE("focus.observer_background_evidence_expires_at_budget", "[component][focus][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 79U, .creation_time_100ns = 1008U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::background}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{79U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::background;
    }));
    REQUIRE(source->wait_until_blocked());
    {
        const auto guard = value->lock_presentation();
        CHECK(!guard.capture_foreground().has_value());
    }

    clock.advance(250ms);
    const auto expired = value->published();
    CHECK(expired.state == evidence::unavailable);
    CHECK(expired.revision == 2U);
    CHECK(expired.observation_started == std::chrono::steady_clock::time_point{});

    source->release();
    value->stop();
}

TEST_CASE("focus.observer_blocked_source_expiry_precedes_stale_result_and_recovers",
    "[component][focus][freshness][concurrency]") {
    manual_clock clock{};
    const process_instance process{.pid = 72U, .creation_time_100ns = 1002U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{72U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    REQUIRE(source->block_next_observation());

    clock.advance(250ms);
    const auto expired_while_blocked = value->published();
    CHECK(expired_while_blocked.state == evidence::unavailable);
    CHECK(expired_while_blocked.revision == 2U);
    CHECK(expired_while_blocked.completed_observations == 1U);

    source->release_initial_observation();
    REQUIRE(source->wait_until_next_observation_blocked());
    REQUIRE(source->wait_for_returned(2U));
    const auto after_slow_result = value->published();
    CHECK(after_slow_result.state == evidence::unavailable);
    CHECK(after_slow_result.revision == expired_while_blocked.revision);
    CHECK(after_slow_result.completed_observations == 2U);

    source->release();
    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 3U && published.state == evidence::foreground;
    }));
    value->stop();
}

TEST_CASE("focus.observer_worker_expires_unseen_gap_before_accepting_slow_result",
    "[component][focus][freshness][concurrency]") {
    manual_clock clock{};
    const process_instance process{.pid = 73U, .creation_time_100ns = 1003U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{73U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    REQUIRE(source->block_next_observation());
    clock.advance(250ms);
    source->release_initial_observation();

    REQUIRE(source->wait_until_next_observation_blocked());
    REQUIRE(source->wait_for_returned(2U));
    const auto expired_before_recovery = value->published();
    CHECK(expired_before_recovery.state == evidence::unavailable);
    CHECK(expired_before_recovery.revision == 2U);
    CHECK(expired_before_recovery.completed_observations == 2U);

    source->release();
    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 3U && published.state == evidence::foreground;
    }));
    value->stop();
}

TEST_CASE("focus.guard_moves_keep_gate_and_captured_freshness_independent",
    "[component][focus][freshness][concurrency]") {
    manual_clock clock{};
    const process_instance process{.pid = 74U, .creation_time_100ns = 1004U};
    auto first_owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const first_source = first_owned_source.get();
    const auto first =
        make_observer(clock, make_incarnation(std::byte{74U}), std::move(first_owned_source));
    const source_release first_release{*first_source};

    auto second_owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const second_source = second_owned_source.get();
    const auto second =
        make_observer(clock, make_incarnation(std::byte{75U}), std::move(second_owned_source));
    const source_release second_release{*second_source};

    REQUIRE(wait_for_publication(*first, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(wait_for_publication(*second, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(first_source->wait_until_blocked());
    REQUIRE(second_source->wait_until_blocked());
    REQUIRE(first_source->block_next_observation());
    const auto captured = first->published();
    const auto other_incarnation = second->published().observer_incarnation;

    {
        auto first_guard = first->lock_presentation();
        auto destination_guard = second->lock_presentation();
        destination_guard = std::move(first_guard);

        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): verify moved-from guards stay inert.
        const auto moved_from_snapshot = first_guard.published();
        const bool moved_from_match = first_guard.matches_foreground(captured);
        const auto moved_from_capture = first_guard.capture_foreground();
        CHECK(!moved_from_snapshot.open);
        CHECK(!moved_from_match);
        CHECK(!moved_from_capture.has_value());
        CHECK(destination_guard.published().observer_incarnation == captured.observer_incarnation);
        CHECK(second->published().observer_incarnation == other_incarnation);

        auto& same_guard = destination_guard;
        destination_guard = std::move(same_guard);
        CHECK(destination_guard.matches_foreground(captured));

        first_source->release_initial_observation();
        REQUIRE(first_source->wait_for_returned(2U));
        clock.advance(250ms);
        CHECK(!destination_guard.matches_foreground(captured));
        const auto held_guard_expiry = destination_guard.published();
        CHECK(held_guard_expiry.state == evidence::unavailable);
        CHECK(held_guard_expiry.revision == 2U);
    }

    REQUIRE(first_source->wait_until_next_observation_blocked());
    const auto after_delayed_publication = first->published();
    CHECK(after_delayed_publication.state == evidence::unavailable);
    CHECK(after_delayed_publication.revision == 2U);
    CHECK(after_delayed_publication.completed_observations == 2U);

    first_source->release();
    second_source->release();
    REQUIRE(wait_for_publication(*first, [](snapshot published) {
        return published.revision == 3U && published.state == evidence::foreground;
    }));
    first->stop();
    second->stop();
}

TEST_CASE("focus.observer_same_revision_refresh_does_not_extend_captured_lease",
    "[component][focus][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 76U, .creation_time_100ns = 1005U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{76U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    REQUIRE(source->block_next_observation());
    const auto captured = value->published();

    clock.advance(200ms);
    source->release_initial_observation();
    REQUIRE(source->wait_until_next_observation_blocked());
    source->release();
    const auto refreshed_at = clock.now();
    REQUIRE(wait_for_publication(*value, [refreshed_at](snapshot published) {
        return published.completed_observations >= 3U &&
               published.observation_started == refreshed_at;
    }));

    {
        const auto guard = value->lock_presentation();
        REQUIRE(guard.published().observation_started == refreshed_at);
        clock.advance(60ms);
        CHECK(!guard.matches_foreground(captured));
        const auto current = guard.published();
        CHECK(current.state == evidence::foreground);
        CHECK(current.revision == captured.revision);
        CHECK(current.observation_started == refreshed_at);
    }
    value->stop();
}

TEST_CASE("focus.observer_expiry_revision_exhaustion_is_sticky", "[component][focus][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 77U, .creation_time_100ns = 1006U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        2U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{77U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    REQUIRE(source->wait_until_blocked());
    auto seed = value->published();
    seed.revision = std::numeric_limits<std::uint64_t>::max();
    detail::observer_test_access::seed_counters(*value, seed);

    clock.advance(250ms);
    const auto exhausted = value->published();
    CHECK(exhausted.exhausted);
    CHECK(exhausted.state == evidence::unavailable);
    CHECK(exhausted.revision == std::numeric_limits<std::uint64_t>::max());
    {
        const auto guard = value->lock_presentation();
        CHECK(!guard.capture_foreground().has_value());
    }

    source->release();
    REQUIRE(source->wait_for_returned(2U));
    const auto after_late_result = value->published();
    CHECK(after_late_result.exhausted);
    CHECK(after_late_result.state == evidence::unavailable);
    CHECK(after_late_result.revision == std::numeric_limits<std::uint64_t>::max());
    value->stop();
}

TEST_CASE("focus.observer_slow_first_observation_is_rejected_before_fresh_recovery",
    "[component][focus][freshness][concurrency]") {
    manual_clock clock{};
    const process_instance process{.pid = 78U, .creation_time_100ns = 1007U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{reading{.process = process, .state = evidence::foreground}},
        1U);
    auto* const source = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{78U}), std::move(owned_source));
    const source_release release{*source};

    REQUIRE(source->wait_until_blocked());
    REQUIRE(source->block_next_observation());
    clock.advance(250ms);
    source->release_initial_observation();
    REQUIRE(source->wait_until_next_observation_blocked());
    REQUIRE(source->wait_for_returned(1U));
    const auto rejected_slow_result = value->published();
    CHECK(rejected_slow_result.state == evidence::unavailable);
    CHECK(rejected_slow_result.revision == 0U);
    CHECK(rejected_slow_result.completed_observations == 1U);

    source->release();
    REQUIRE(wait_for_publication(*value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    CHECK(value->published().observation_started == clock.now());
    value->stop();
}

} // namespace
