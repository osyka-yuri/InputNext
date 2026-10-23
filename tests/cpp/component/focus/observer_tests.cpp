#include <inputnext/focus.hpp>

#include "focus/observer_test_access.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
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

[[nodiscard]] std::unique_ptr<observer>
make_observer(manual_clock& clock, runtime_id incarnation, std::unique_ptr<source> owned_source) {
    return detail::observer_test_access::make_with_clock(incarnation,
        std::move(owned_source),
        &manual_clock::read,
        &clock);
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

class worker_cleanup final {
public:
    worker_cleanup(controlled_source& source, std::jthread& worker) noexcept
        : source_(source),
          worker_(worker) {}

    ~worker_cleanup() {
        source_.release();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    controlled_source& source_;
    std::jthread& worker_;
};

TEST_CASE("focus.observer_blocked_poll_preserves_publication_and_discards_after_stop",
    "[component][focus][concurrency]") {
    manual_clock clock{};
    std::atomic<std::size_t> destructions{};
    {
        const process_instance process{.pid = 41U, .creation_time_100ns = 123U};
        const std::vector readings{
            reading{.process = process, .state = evidence::background, .native_code = 1U},
            reading{.process = process, .state = evidence::foreground, .native_code = 2U},
        };
        auto owned_source =
            std::make_unique<controlled_source>(process, readings, 2U, &destructions);
        auto* const source = owned_source.get();
        const auto owned_observer =
            make_observer(clock, make_incarnation(std::byte{1U}), std::move(owned_source));
        auto& value = *owned_observer;
        std::jthread stopper;
        const worker_cleanup cleanup{*source, stopper};

        REQUIRE(wait_for_publication(value, [](snapshot published) {
            return published.open && published.revision == 1U &&
                   published.state == evidence::background;
        }));
        REQUIRE(source->wait_until_blocked());

        const auto captured = value.published();
        CHECK(captured.completed_observations == 1U);
        CHECK(captured.native_code == 1U);
        CHECK(destructions.load(std::memory_order_relaxed) == 0U);

        runtime_id successor_incarnation = make_incarnation(std::byte{2U});
        auto successor_source = std::make_unique<controlled_source>(process,
            std::vector{
                reading{.process = process, .state = evidence::foreground, .native_code = 3U}});
        const auto owned_successor =
            make_observer(clock, successor_incarnation, std::move(successor_source));
        auto& successor = *owned_successor;
        REQUIRE(wait_for_publication(successor, [](snapshot published) {
            return published.open && published.revision == 1U &&
                   published.state == evidence::foreground;
        }));
        {
            const auto guard = successor.lock_presentation();
            CHECK(!guard.matches_foreground(captured));
        }

        std::atomic<bool> stop_started{};
        std::atomic<bool> stop_finished{};
        stopper = std::jthread{[&] {
            stop_started.store(true, std::memory_order_release);
            value.stop();
            stop_finished.store(true, std::memory_order_release);
        }};
        REQUIRE(wait_for_publication(value, [](snapshot published) { return !published.open; }));
        CHECK(stop_started.load(std::memory_order_acquire));
        CHECK(!stop_finished.load(std::memory_order_acquire));
        CHECK(destructions.load(std::memory_order_relaxed) == 0U);

        source->release();
        stopper.join();
        CHECK(stop_finished.load(std::memory_order_acquire));
        CHECK(destructions.load(std::memory_order_relaxed) == 0U);
        const auto closed = value.published();
        CHECK(!closed.open);
        CHECK(closed.revision == captured.revision);
        CHECK(closed.state == evidence::background);
        CHECK(closed.native_code == captured.native_code);
        CHECK(closed.completed_observations == captured.completed_observations);
        {
            const auto guard = value.lock_presentation();
            CHECK(!guard.capture_foreground().has_value());
        }
        const auto successor_after_release = successor.published();
        CHECK(successor_after_release.observer_incarnation == successor_incarnation);
        CHECK(successor_after_release.state == evidence::foreground);

        value.stop();
        successor.stop();
    }
    CHECK(destructions.load(std::memory_order_relaxed) == 1U);
}

TEST_CASE("focus.observer_revision_changes_only_with_evidence", "[component][focus]") {
    manual_clock clock{};
    const process_instance process{.pid = 42U, .creation_time_100ns = 456U};
    const std::vector readings{
        reading{.process = process, .state = evidence::foreground, .native_code = 10U},
        reading{.process = process, .state = evidence::foreground, .native_code = 11U},
        reading{.process = process, .state = evidence::background, .native_code = 12U},
    };
    auto owned_source = std::make_unique<controlled_source>(process, readings, 2U);
    auto* const source = owned_source.get();
    const auto owned_observer =
        make_observer(clock, make_incarnation(std::byte{3U}), std::move(owned_source));
    auto& value = *owned_observer;
    std::jthread stopper;
    const worker_cleanup cleanup{*source, stopper};

    REQUIRE(wait_for_publication(value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::foreground;
    }));
    const auto foreground = value.published();
    {
        const auto guard = value.lock_presentation();
        CHECK(guard.matches_foreground(foreground));
    }
    REQUIRE(source->wait_until_blocked());
    CHECK(value.published().revision == foreground.revision);
    REQUIRE(source->block_next_observation());
    source->release_initial_observation();
    REQUIRE(source->wait_until_next_observation_blocked());

    const auto repeated = value.published();
    CHECK(repeated.revision == foreground.revision);
    CHECK(repeated.state == evidence::foreground);
    CHECK(repeated.native_code == 11U);
    CHECK(repeated.completed_observations == 2U);

    source->release();
    REQUIRE(wait_for_publication(value, [](snapshot published) {
        return published.revision == 2U && published.state == evidence::background;
    }));
    {
        const auto guard = value.lock_presentation();
        CHECK(!guard.matches_foreground(foreground));
    }
    value.stop();
}

TEST_CASE("focus.observer_process_mismatch_fails_closed_and_revision_exhaustion_is_sticky",
    "[component][focus][concurrency]") {
    manual_clock clock{};
    const process_instance process{.pid = 43U, .creation_time_100ns = 789U};
    const process_instance other_process{.pid = 44U, .creation_time_100ns = 987U};
    const std::vector readings{
        reading{.process = process, .state = evidence::background, .native_code = 20U},
        reading{.process = other_process, .state = evidence::foreground, .native_code = 21U},
        reading{.process = process, .state = evidence::foreground, .native_code = 22U},
    };
    auto owned_source = std::make_unique<controlled_source>(process, readings, 2U);
    auto* const source = owned_source.get();
    const auto owned_observer =
        make_observer(clock, make_incarnation(std::byte{4U}), std::move(owned_source));
    auto& value = *owned_observer;
    std::jthread stopper;
    const worker_cleanup cleanup{*source, stopper};
    REQUIRE(wait_for_publication(value, [](snapshot published) {
        return published.revision == 1U && published.state == evidence::background;
    }));
    REQUIRE(source->wait_until_blocked());

    auto seed = value.published();
    seed.revision = std::numeric_limits<std::uint64_t>::max();
    seed.completed_observations = std::numeric_limits<std::uint64_t>::max();
    detail::observer_test_access::seed_counters(value, seed);
    source->release();
    REQUIRE(wait_for_publication(value, [](snapshot published) { return published.exhausted; }));
    REQUIRE(source->wait_for_returned(3U));
    REQUIRE(wait_for_publication(value,
        [](snapshot published) { return published.exhausted && published.native_code == 22U; }));

    const auto exhausted = value.published();
    CHECK(exhausted.revision == std::numeric_limits<std::uint64_t>::max());
    CHECK(exhausted.completed_observations == std::numeric_limits<std::uint64_t>::max());
    CHECK(exhausted.state == evidence::unavailable);
    CHECK(exhausted.process == process);
    {
        const auto guard = value.lock_presentation();
        CHECK(!guard.matches_foreground(exhausted));
    }
    value.stop();
}

TEST_CASE("focus.observer_rejects_invalid_incarnation_source_and_process", "[component][focus]") {
    const process_instance process{.pid = 45U, .creation_time_100ns = 1000U};
    const auto valid_reading =
        reading{.process = process, .state = evidence::unavailable, .native_code = 0U};
    const auto valid_source = [&] {
        return std::make_unique<controlled_source>(process, std::vector{valid_reading});
    };

    CHECK_THROWS_AS((observer{runtime_id{}, valid_source()}), std::invalid_argument);
    CHECK_THROWS_AS((observer{make_incarnation(std::byte{5U}), nullptr}), std::invalid_argument);

    const process_instance invalid_process{};
    auto invalid_source = std::make_unique<controlled_source>(invalid_process,
        std::vector{
            reading{.process = invalid_process, .state = evidence::foreground, .native_code = 0U}});
    CHECK_THROWS_AS((observer{make_incarnation(std::byte{6U}), std::move(invalid_source)}),
        std::invalid_argument);
}

} // namespace
