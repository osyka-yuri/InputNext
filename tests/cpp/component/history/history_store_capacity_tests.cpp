#include "history_store_test_support.hpp"

#include "state.hpp"
#include "store.hpp"
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace inputnext::history_component_test {
namespace {

class opening_cleanup final {
public:
    opening_cleanup(fake_history_provider& provider,
        std::shared_ptr<fake_cursor_state>& cursor,
        std::jthread& worker) noexcept
        : provider_(provider),
          cursor_(cursor),
          worker_(worker) {}

    ~opening_cleanup() {
        provider_.release_blocked_factory();
        if (cursor_ != nullptr) {
            cursor_->release_destruction();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    opening_cleanup(const opening_cleanup&) = delete;
    opening_cleanup& operator=(const opening_cleanup&) = delete;
    opening_cleanup(opening_cleanup&&) = delete;
    opening_cleanup& operator=(opening_cleanup&&) = delete;

private:
    fake_history_provider& provider_;
    std::shared_ptr<fake_cursor_state>& cursor_;
    std::jthread& worker_;
};

} // namespace

TEST_CASE("history.store.reader_capacity_is_finite_and_handles_are_not_reused",
    "[component][history]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(10U);
    std::array<history_reader_handle, public_history_reader_capacity> readers{};

    for (auto& reader : readers) {
        const auto opened = store.open(target, test_now);
        REQUIRE(opened.has_value());
        reader = *opened;
    }

    const auto full = store.open(target, test_now);
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().code == history_error::capacity_exceeded);

    const auto stale = readers[public_history_reader_capacity / 2U];
    REQUIRE(store.close(stale).has_value());
    const auto stale_read = store.read(stale, test_now);
    if (stale_read.failure.has_value()) {
        CHECK(stale_read.failure->code == history_error::invalid_reader);
    } else {
        FAIL("stale reader must report its invalid handle");
    }

    const auto reopened = store.open(target, test_now);
    REQUIRE(reopened.has_value());
    CHECK(reopened->value != stale.value);
    CHECK(store.resident_bank_count() == 1U);
}

TEST_CASE("history.store.resident_capacity_recovers_after_retirement", "[component][history]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};

    for (std::uint64_t index = 0U; index < resident_history_bank_capacity; ++index) {
        const auto opened = store.open(make_target(100U + index), test_now);
        REQUIRE(opened.has_value());
        REQUIRE(store.close(*opened).has_value());
    }
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);

    const auto unavailable_target = make_target(200U);
    const auto unavailable = store.open(unavailable_target, test_now);
    REQUIRE_FALSE(unavailable.has_value());
    CHECK(unavailable.error().code == history_error::capacity_exceeded);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == resident_history_bank_capacity);

    const auto retired_target = make_target(100U);
    store.retire(retired_target);
    CHECK(store.drain_retired() == 1U);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 1U);

    const auto recovered = store.open(unavailable_target, test_now);
    REQUIRE(recovered.has_value());
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
}

TEST_CASE("history.store.opening_reservations_enforce_capacity_until_disposal",
    "[component][history][capacity][concurrency]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    for (std::size_t index = 0U; index < resident_history_bank_capacity - 1U; ++index) {
        const auto opened = store.open(make_target(1'000U + index), test_now);
        REQUIRE(opened.has_value());
        REQUIRE(store.close(*opened).has_value());
    }
    REQUIRE(store.resident_bank_count() == resident_history_bank_capacity - 1U);

    const auto opening_target = make_target(2'000U);
    provider.block_next_factory_return();
    auto opening_error = history_error::provider_failure;
    auto opening_succeeded = false;
    auto factory_cursor = std::shared_ptr<fake_cursor_state>{};
    auto worker = std::jthread{};
    const auto cleanup = opening_cleanup{provider, factory_cursor, worker};
    worker = std::jthread{[&] {
        const auto result = store.open(opening_target, test_now);
        opening_succeeded = result.has_value();
        if (!opening_succeeded) {
            opening_error = result.error().code;
        }
    }};

    REQUIRE(provider.wait_until_factory_blocked(std::chrono::seconds{2}));
    factory_cursor = provider.cursor_state(opening_target);
    REQUIRE(factory_cursor != nullptr);
    factory_cursor->block_destruction();

    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == resident_history_bank_capacity);

    const auto duplicate = store.open(opening_target, test_now);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == history_error::busy);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == resident_history_bank_capacity);

    const auto unavailable_target = make_target(2'001U);
    const auto unavailable = store.open(unavailable_target, test_now);
    REQUIRE_FALSE(unavailable.has_value());
    CHECK(unavailable.error().code == history_error::capacity_exceeded);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == resident_history_bank_capacity);

    store.retire(opening_target);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
    provider.release_blocked_factory();
    REQUIRE(factory_cursor->wait_until_destroying(std::chrono::seconds{2}));

    const auto held_for_disposal = store.open(unavailable_target, test_now);
    REQUIRE_FALSE(held_for_disposal.has_value());
    CHECK(held_for_disposal.error().code == history_error::capacity_exceeded);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == resident_history_bank_capacity);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 0U);

    factory_cursor->release_destruction();
    if (worker.joinable()) {
        worker.join();
    }
    CHECK(!opening_succeeded);
    CHECK(opening_error == history_error::disconnected);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 1U);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity - 1U);

    const auto recovered = store.open(unavailable_target, test_now);
    REQUIRE(recovered.has_value());
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) ==
          resident_history_bank_capacity + 1U);
}

TEST_CASE("history.store.factory_failure_releases_reservation_for_retry", "[component][history]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(11U);
    provider.fail_next_factory(history_error::provider_failure);

    const auto failed = store.open(target, test_now);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == history_error::provider_failure);
    CHECK(store.resident_bank_count() == 0U);

    const auto retried = store.open(target, test_now);
    REQUIRE(retried.has_value());
    CHECK(retried->value > 1U);
    CHECK(store.resident_bank_count() == 1U);
}

} // namespace inputnext::history_component_test
