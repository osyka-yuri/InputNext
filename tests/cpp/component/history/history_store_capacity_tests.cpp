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
#include <limits>
#include <memory>
#include <thread>
#include <vector>

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

TEST_CASE("history.store.reserved_xinput_readers_are_independent_from_public_capacity",
    "[component][history][capacity]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(12U);
    std::vector<history_reader_handle> public_readers(public_history_reader_capacity);
    std::vector<history_reader_handle> reserved_readers(reserved_xinput_history_reader_capacity);

    for (auto& reader : public_readers) {
        const auto opened = store.open(target, test_now);
        REQUIRE(opened.has_value());
        reader = *opened;
    }
    for (std::size_t position = 0U; position < reserved_readers.size(); ++position) {
        const auto opened =
            store.open_xinput_reader(static_cast<std::uint32_t>(position), target, test_now);
        REQUIRE(opened.has_value());
        reserved_readers[position] = *opened;
    }

    const auto duplicate_position = store.open_xinput_reader(0U, target, test_now);
    REQUIRE(!duplicate_position.has_value());
    CHECK(duplicate_position.error().code == history_error::busy);

    const auto invalid_position = store.open_xinput_reader(
        static_cast<std::uint32_t>(reserved_xinput_history_reader_capacity),
        target,
        test_now);
    REQUIRE(!invalid_position.has_value());
    CHECK(invalid_position.error().code == history_error::invalid_argument);

    const auto public_full = store.open(target, test_now);
    REQUIRE(!public_full.has_value());
    CHECK(public_full.error().code == history_error::capacity_exceeded);

    std::array<std::uint64_t,
        public_history_reader_capacity + reserved_xinput_history_reader_capacity>
        streams{};
    std::size_t stream_count{};
    for (const auto reader : public_readers) {
        const auto baseline = std::make_unique<history_batch>(store.read(reader, test_now));
        REQUIRE(baseline->count == 1U);
        CHECK(!baseline->failure.has_value());
        CHECK(baseline->reset == history_reset::initial_open);
        streams[stream_count++] = baseline->domain.stream;
    }
    for (const auto reader : reserved_readers) {
        const auto baseline = std::make_unique<history_batch>(store.read(reader, test_now));
        REQUIRE(baseline->count == 1U);
        CHECK(!baseline->failure.has_value());
        CHECK(baseline->reset == history_reset::initial_open);
        streams[stream_count++] = baseline->domain.stream;
    }
    for (std::size_t left = 0U; left < streams.size(); ++left) {
        for (std::size_t right = left + 1U; right < streams.size(); ++right) {
            CHECK(streams[left] != streams[right]);
        }
    }

    const auto stale_reserved = reserved_readers[0U];
    REQUIRE(store.close(stale_reserved).has_value());
    const auto reopened_reserved = store.open_xinput_reader(0U, target, test_now);
    REQUIRE(reopened_reserved.has_value());
    CHECK(reopened_reserved->value != stale_reserved.value);
    const auto stale_close = store.close(stale_reserved);
    REQUIRE(!stale_close.has_value());
    CHECK(stale_close.error().code == history_error::invalid_reader);
    const auto reopened_baseline =
        std::make_unique<history_batch>(store.read(*reopened_reserved, test_now));
    REQUIRE(reopened_baseline->count == 1U);
    CHECK(reopened_baseline->records[0U].baseline);

    const auto stale_public = public_readers[0U];
    REQUIRE(store.close(stale_public).has_value());
    const auto reopened_public = store.open(target, test_now);
    REQUIRE(reopened_public.has_value());
    CHECK(reopened_public->value != stale_public.value);
    CHECK(store.read(stale_public, test_now).failure ==
          history_failure{.code = history_error::invalid_reader});

    for (std::size_t index = 1U; index < public_readers.size(); ++index) {
        REQUIRE(store.close(public_readers[index]).has_value());
    }
    REQUIRE(store.close(*reopened_public).has_value());
    for (std::size_t position = 1U; position < reserved_readers.size(); ++position) {
        REQUIRE(store.close(reserved_readers[position]).has_value());
    }
    REQUIRE(store.close(*reopened_reserved).has_value());
}

TEST_CASE("history.store.reserved_reader_identity_does_not_wrap", "[component][history]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto provider = fake_history_provider{};
    auto store =
        history_store{provider, identity_seed{.next_reader = maximum, .next_stream = maximum}};
    const auto target = make_target(13U);

    const auto opened = store.open_xinput_reader(2U, target, test_now);
    REQUIRE(opened.has_value());
    CHECK(opened->value == maximum);
    const auto baseline = store.read(*opened, test_now);
    REQUIRE(baseline.count == 1U);
    CHECK(baseline.domain.stream == maximum);

    const auto exhausted_public = store.open(make_target(14U), test_now);
    REQUIRE(!exhausted_public.has_value());
    CHECK(exhausted_public.error().code == history_error::identity_exhausted);
    REQUIRE(store.close(*opened).has_value());
    const auto exhausted_reserved = store.open_xinput_reader(2U, target, test_now);
    REQUIRE(!exhausted_reserved.has_value());
    CHECK(exhausted_reserved.error().code == history_error::identity_exhausted);
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

TEST_CASE(
    "history.store.reserved_opening_and_bank_reservations_survive_rejected_cursor_destruction",
    "[component][history][capacity][concurrency]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    for (std::size_t index = 0U; index < resident_history_bank_capacity - 1U; ++index) {
        const auto opened = store.open(make_target(3'000U + index), test_now);
        REQUIRE(opened.has_value());
        REQUIRE(store.close(*opened).has_value());
    }
    REQUIRE(store.resident_bank_count() == resident_history_bank_capacity - 1U);

    const auto opening_target = make_target(4'000U);
    provider.block_next_factory_return();
    auto opening_error = history_error::provider_failure;
    auto opening_succeeded = false;
    auto factory_cursor = std::shared_ptr<fake_cursor_state>{};
    auto worker = std::jthread{};
    const auto cleanup = opening_cleanup{provider, factory_cursor, worker};
    worker = std::jthread{[&] {
        const auto result = store.open_xinput_reader(3U, opening_target, test_now);
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

    const auto occupied_position = store.open_xinput_reader(3U, opening_target, test_now);
    REQUIRE(!occupied_position.has_value());
    CHECK(occupied_position.error().code == history_error::busy);
    const auto other_position_same_target = store.open_xinput_reader(2U, opening_target, test_now);
    REQUIRE(!other_position_same_target.has_value());
    CHECK(other_position_same_target.error().code == history_error::busy);
    const auto other_target = store.open(make_target(4'001U), test_now);
    REQUIRE(!other_target.has_value());
    CHECK(other_target.error().code == history_error::capacity_exceeded);

    store.retire(opening_target);
    provider.release_blocked_factory();
    REQUIRE(factory_cursor->wait_until_destroying(std::chrono::seconds{2}));

    const auto retained_position = store.open_xinput_reader(3U, opening_target, test_now);
    REQUIRE(!retained_position.has_value());
    CHECK(retained_position.error().code == history_error::busy);
    const auto retained_same_target = store.open(opening_target, test_now);
    REQUIRE(!retained_same_target.has_value());
    CHECK(retained_same_target.error().code == history_error::busy);
    const auto retained_bank = store.open(make_target(4'002U), test_now);
    REQUIRE(!retained_bank.has_value());
    CHECK(retained_bank.error().code == history_error::capacity_exceeded);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 0U);

    factory_cursor->release_destruction();
    if (worker.joinable()) {
        worker.join();
    }
    CHECK(!opening_succeeded);
    CHECK(opening_error == history_error::disconnected);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 1U);
    CHECK(store.resident_bank_count() == resident_history_bank_capacity - 1U);

    const auto recovered = store.open_xinput_reader(3U, make_target(4'003U), test_now);
    REQUIRE(recovered.has_value());
    CHECK(store.resident_bank_count() == resident_history_bank_capacity);
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
