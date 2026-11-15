#include "focus/observer_test_access.hpp"

#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

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

[[nodiscard]] runtime_id make_incarnation(std::byte value) noexcept {
    runtime_id result{};
    result.bytes[0U] = value;
    return result;
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

[[nodiscard]] reading window_reading(process_instance process,
    window_id window,
    std::uint64_t transition,
    bool available = true) noexcept {
    return reading{.process = process,
        .state = evidence::foreground,
        .foreground_window = window,
        .window_transition = transition,
        .window_available = available};
}

[[nodiscard]] std::unique_ptr<observer>
make_observer(manual_clock& clock, runtime_id incarnation, std::unique_ptr<source> owned_source) {
    return detail::observer_test_access::make_with_clock(incarnation,
        std::move(owned_source),
        &manual_clock::read,
        &clock);
}

TEST_CASE("focus.same_process_window_change_revokes_only_window_epoch",
    "[component][focus][window]") {
    manual_clock clock{};
    const process_instance process{.pid = 310U, .creation_time_100ns = 9910U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{window_reading(process, 0x100U, 0U), window_reading(process, 0x200U, 1U)},
        2U);
    auto* const provider = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{31U}), std::move(owned_source));
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.state == evidence::foreground && current.window_available &&
               current.foreground_window == 0x100U;
    }));
    REQUIRE(provider->wait_until_blocked());

    const auto captured = value->published();
    foreground_epoch process_epoch{};
    window_epoch old_window_epoch{};
    {
        const auto guard = value->lock_presentation();
        process_epoch = guard.current_foreground_epoch().value_or(foreground_epoch{});
        old_window_epoch = guard.current_foreground_window_epoch(0x100U).value_or(window_epoch{});
    }
    REQUIRE(process_epoch.revision != 0U);
    REQUIRE(old_window_epoch.revision != 0U);

    provider->release_initial_observation();
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.completed_observations >= 2U && current.window_available &&
               current.foreground_window == 0x200U;
    }));
    {
        const auto guard = value->lock_presentation();
        CHECK(guard.current_foreground_epoch() == process_epoch);
        CHECK(guard.matches_foreground(captured));
        CHECK_FALSE(guard.matches_foreground_window_epoch(old_window_epoch));
        const auto new_epoch =
            guard.current_foreground_window_epoch(0x200U).value_or(window_epoch{});
        REQUIRE(new_epoch.revision != 0U);
        CHECK(new_epoch.revision == old_window_epoch.revision + 1U);
    }
    provider->release_initial_observation();
    value->stop();
}

TEST_CASE("focus.window_transition_serial_catches_unobserved_a_to_b_to_a",
    "[component][focus][window]") {
    manual_clock clock{};
    const process_instance process{.pid = 311U, .creation_time_100ns = 9911U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{window_reading(process, 0x300U, 4U), window_reading(process, 0x300U, 6U)},
        2U);
    auto* const provider = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{32U}), std::move(owned_source));
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.window_available && current.foreground_window == 0x300U &&
               current.window_transition == 4U;
    }));
    REQUIRE(provider->wait_until_blocked());

    window_epoch captured{};
    {
        const auto guard = value->lock_presentation();
        captured = guard.current_foreground_window_epoch(0x300U).value_or(window_epoch{});
    }
    REQUIRE(captured.revision != 0U);

    provider->release_initial_observation();
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.completed_observations >= 2U && current.window_transition == 6U;
    }));
    const auto current = value->published();
    CHECK(current.foreground_window == captured.window);
    CHECK(current.revision == captured.process.revision);
    CHECK(current.window_revision == captured.revision + 1U);
    {
        const auto guard = value->lock_presentation();
        CHECK_FALSE(guard.matches_foreground_window_epoch(captured));
        const auto current_epoch =
            guard.current_foreground_window_epoch(0x300U).value_or(window_epoch{});
        REQUIRE(current_epoch.revision != 0U);
        CHECK(current_epoch.revision == captured.revision + 1U);
    }
    provider->release_initial_observation();
    value->stop();
}

TEST_CASE("focus.window_provider_failure_does_not_revoke_process_evidence",
    "[component][focus][window]") {
    manual_clock clock{};
    const process_instance process{.pid = 312U, .creation_time_100ns = 9912U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{window_reading(process, 0x400U, 0U), window_reading(process, 0U, 0U, false)},
        2U);
    auto* const provider = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{33U}), std::move(owned_source));
    REQUIRE(
        wait_for_publication(*value, [](snapshot current) { return current.window_available; }));
    REQUIRE(provider->wait_until_blocked());

    const auto captured = value->published();
    const auto window_revision = captured.window_revision;
    provider->release_initial_observation();
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.completed_observations >= 2U && !current.window_available;
    }));
    const auto after_failure = value->published();
    CHECK(after_failure.state == evidence::foreground);
    CHECK(after_failure.revision == captured.revision);
    CHECK(after_failure.window_revision == window_revision + 1U);
    CHECK_FALSE(after_failure.window_exhausted);
    {
        const auto guard = value->lock_presentation();
        CHECK(guard.matches_foreground(captured));
    }
    provider->release_initial_observation();
    value->stop();
}

TEST_CASE("focus.window_expiry_and_recovery_are_independent_of_process_freshness",
    "[component][focus][window][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 313U, .creation_time_100ns = 9913U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{window_reading(process, 0x500U, 8U)},
        2U);
    auto* const provider = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{34U}), std::move(owned_source));
    REQUIRE(
        wait_for_publication(*value, [](snapshot current) { return current.window_available; }));
    REQUIRE(provider->wait_until_blocked());

    auto seed = value->published();
    seed.window_observation_started = clock.now() - 250ms;
    detail::observer_test_access::seed_window_state(*value, seed);
    {
        const auto guard = value->lock_presentation();
        const auto expired = guard.published();
        CHECK(expired.state == evidence::foreground);
        CHECK(expired.revision == seed.revision);
        CHECK_FALSE(expired.window_available);
        CHECK(expired.window_revision == seed.window_revision + 1U);
        CHECK(guard.current_foreground_epoch().has_value());
        CHECK_FALSE(guard.current_foreground_window_epoch(0x500U).has_value());
    }

    provider->release_initial_observation();
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.completed_observations >= 2U && current.window_available;
    }));
    const auto recovered = value->published();
    CHECK(recovered.state == evidence::foreground);
    CHECK(recovered.revision == seed.revision);
    CHECK(recovered.window_revision >= seed.window_revision + 2U);
    provider->release_initial_observation();
    value->stop();
}

TEST_CASE("focus.window_revision_exhaustion_does_not_exhaust_process_evidence",
    "[component][focus][window][freshness]") {
    manual_clock clock{};
    const process_instance process{.pid = 314U, .creation_time_100ns = 9914U};
    auto owned_source = std::make_unique<controlled_source>(process,
        std::vector{window_reading(process, 0x600U, 0U), window_reading(process, 0x700U, 1U)},
        2U);
    auto* const provider = owned_source.get();
    const auto value =
        make_observer(clock, make_incarnation(std::byte{35U}), std::move(owned_source));
    REQUIRE(
        wait_for_publication(*value, [](snapshot current) { return current.window_available; }));
    REQUIRE(provider->wait_until_blocked());

    auto seed = value->published();
    seed.window_revision = std::numeric_limits<std::uint64_t>::max();
    detail::observer_test_access::seed_window_state(*value, seed);
    provider->release_initial_observation();
    REQUIRE(wait_for_publication(*value, [](snapshot current) {
        return current.completed_observations >= 2U && current.window_exhausted;
    }));

    const auto exhausted = value->published();
    CHECK(exhausted.window_exhausted);
    CHECK_FALSE(exhausted.window_available);
    CHECK_FALSE(exhausted.exhausted);
    CHECK(exhausted.state == evidence::foreground);
    CHECK(exhausted.revision == seed.revision);
    provider->release_initial_observation();
    value->stop();
}

} // namespace
