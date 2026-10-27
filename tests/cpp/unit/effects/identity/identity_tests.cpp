#include "effects/effects.hpp"
#include "effects/identity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace inputnext::effects_test {
namespace {

template <typename Value>
void check_capacity_exhausted(const effects::result<Value>& actual) {
    if (actual.has_value()) {
        FAIL("Identity source returned a value after exhaustion");
    } else {
        CHECK(actual.error() == effects::status::capacity_exceeded);
    }
}

} // namespace

TEST_CASE("effects.identity_source_emits_max_once_then_stays_exhausted",
    "[unit][effects][identity]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    effects::identity_source identities{effects::identity_seeds{.next_context = maximum - 1U,
        .next_publication = maximum - 1U,
        .next_capability = maximum - 1U}};

    const auto first_context = identities.take_context();
    REQUIRE(first_context.has_value());
    CHECK(*first_context == maximum - 1U);
    const auto last_context = identities.take_context();
    REQUIRE(last_context.has_value());
    CHECK(*last_context == maximum);
    const auto exhausted_context = identities.take_context();
    check_capacity_exhausted(exhausted_context);
    const auto still_exhausted_context = identities.take_context();
    check_capacity_exhausted(still_exhausted_context);

    const auto first_publication = identities.take_publication();
    REQUIRE(first_publication.has_value());
    CHECK(first_publication->value == maximum - 1U);
    const auto last_publication = identities.take_publication();
    REQUIRE(last_publication.has_value());
    CHECK(last_publication->value == maximum);
    const auto exhausted_publication = identities.take_publication();
    check_capacity_exhausted(exhausted_publication);

    const auto first_capability = identities.take_capability();
    REQUIRE(first_capability.has_value());
    CHECK(*first_capability == maximum - 1U);
    const auto last_capability = identities.take_capability();
    REQUIRE(last_capability.has_value());
    CHECK(*last_capability == maximum);
    const auto exhausted_capability = identities.take_capability();
    check_capacity_exhausted(exhausted_capability);
}

TEST_CASE("effects.identity_source_publication_tokens_are_unique_under_concurrency",
    "[unit][effects][identity][concurrency]") {
    constexpr std::size_t thread_count = 8U;
    constexpr std::size_t identities_per_thread = 512U;
    constexpr std::size_t identity_count = thread_count * identities_per_thread;

    effects::identity_source identities;
    std::mutex values_gate;
    std::vector<std::uint64_t> values;
    values.reserve(identity_count);
    std::atomic<bool> failed{};
    std::array<std::thread, thread_count> workers;

    for (auto& worker : workers) {
        worker = std::thread{[&] {
            std::array<std::uint64_t, identities_per_thread> local{};
            for (auto& value : local) {
                const auto next = identities.take_publication();
                if (!next.has_value()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                value = next->value;
            }
            std::scoped_lock lock{values_gate};
            values.insert(values.end(), local.begin(), local.end());
        }};
    }
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(!failed.load(std::memory_order_relaxed));
    REQUIRE(values.size() == identity_count);
    std::ranges::sort(values);
    CHECK(std::ranges::adjacent_find(values) == values.end());
    CHECK(values.front() == 1U);
    CHECK(values.back() == identity_count);
}

} // namespace inputnext::effects_test
