#include "history_store_test_support.hpp"

#include "store.hpp"
#include "support/history/history_store_test_access.hpp"
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

namespace inputnext::history_component_test {
namespace {

class pending_read final {
public:
    pending_read(history_store& store,
        history_reader_handle reader,
        std::shared_ptr<fake_cursor_state> cursor)
        : cursor_(std::move(cursor)),
          thread_([this, &store, reader] { *result_ = store.read(reader, test_now); }) {}

    ~pending_read() {
        cursor_->release_blocked_call();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    pending_read(const pending_read&) = delete;
    pending_read& operator=(const pending_read&) = delete;
    pending_read(pending_read&&) = delete;
    pending_read& operator=(pending_read&&) = delete;

    [[nodiscard]] bool wait_until_blocked() {
        return cursor_->wait_until_blocked(std::chrono::seconds{2});
    }

    [[nodiscard]] history_batch finish() {
        cursor_->release_blocked_call();
        if (thread_.joinable()) {
            thread_.join();
        }
        return *result_;
    }

private:
    std::shared_ptr<fake_cursor_state> cursor_;
    std::unique_ptr<history_batch> result_{std::make_unique<history_batch>()};
    std::thread thread_;
};

void count_hook(void* context) noexcept {
    static_cast<std::atomic<std::size_t>*>(context)->fetch_add(1U, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("history.store.serializes_readers_and_close_waits_for_active_read",
    "[component][history][concurrency]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(20U);
    const auto first_open = store.open(target, test_now);
    const auto second_open = store.open(target, test_now);
    REQUIRE(first_open.has_value());
    REQUIRE(second_open.has_value());

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    REQUIRE(store.read(*first_open, test_now).count == 1U);
    const auto retained_sample = make_sample(500U, 11U, 2U);
    cursor->enqueue_reading(retained_sample);
    cursor->block_next();
    auto first_read = pending_read{store, *first_open, cursor};
    REQUIRE(first_read.wait_until_blocked());

    CHECK(
        store.read(*first_open, test_now).failure == history_failure{.code = history_error::busy});
    CHECK(
        store.read(*second_open, test_now).failure == history_failure{.code = history_error::busy});
    REQUIRE(store.close(*first_open).has_value());
    CHECK(store.read(*first_open, test_now).failure ==
          history_failure{.code = history_error::closed});
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 0U);
    CHECK(store.resident_bank_count() == 1U);

    const auto closed_read = std::make_unique<history_batch>(first_read.finish());
    CHECK(closed_read->count == 0U);
    CHECK(closed_read->failure == history_failure{.code = history_error::closed});
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 0U);

    const auto second_baseline =
        std::make_unique<history_batch>(store.read(*second_open, test_now));
    REQUIRE(second_baseline->count == 1U);
    CHECK(second_baseline->reset == history_reset::initial_open);
    CHECK(second_baseline->records[0U].baseline);

    const auto retained_reading =
        std::make_unique<history_batch>(store.read(*second_open, test_now));
    REQUIRE(retained_reading->count == 1U);
    CHECK(retained_reading->records[0U].sample == retained_sample);
    CHECK(retained_reading->records[0U].sequence == 2U);
}

TEST_CASE("history.store.snapshots_read_commit_hook_at_reservation",
    "[component][history][concurrency]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(31U);
    const auto opened = store.open(target, test_now);
    REQUIRE(opened.has_value());
    REQUIRE(store.read(*opened, test_now).count == 1U);

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    cursor->enqueue_reading(make_sample(500U, 11U));
    cursor->block_next();
    std::atomic<std::size_t> reserved_hook_calls{};
    std::atomic<std::size_t> replacement_hook_calls{};
    history_store_test_access::set_before_read_commit_test_hook(store,
        count_hook,
        &reserved_hook_calls);

    auto read = pending_read{store, *opened, cursor};
    REQUIRE(read.wait_until_blocked());
    history_store_test_access::set_before_read_commit_test_hook(store,
        count_hook,
        &replacement_hook_calls);
    const auto batch = std::make_unique<history_batch>(read.finish());

    CHECK(batch->count == 1U);
    CHECK(reserved_hook_calls.load(std::memory_order_relaxed) == 1U);
    CHECK(replacement_hook_calls.load(std::memory_order_relaxed) == 0U);
    history_store_test_access::set_before_read_commit_test_hook(store, nullptr, nullptr);
}

TEST_CASE("history.store.retirement_waits_for_active_read", "[component][history][concurrency]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(21U);
    const auto opened = store.open(target, test_now);
    REQUIRE(opened.has_value());

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    REQUIRE(store.read(*opened, test_now).count == 1U);
    cursor->block_next();
    auto active_read = pending_read{store, *opened, cursor};
    REQUIRE(active_read.wait_until_blocked());

    store.retire(target);
    CHECK(store.drain_retired() == 0U);
    CHECK(store.resident_bank_count() == 1U);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 0U);

    const auto disconnected_read = std::make_unique<history_batch>(active_read.finish());
    CHECK(disconnected_read->count == 0U);
    CHECK(disconnected_read->failure == history_failure{.code = history_error::disconnected});
    CHECK(store.drain_retired() == 1U);
    CHECK(store.resident_bank_count() == 0U);
    CHECK(provider.cursor_destructions.load(std::memory_order_relaxed) == 1U);
}

TEST_CASE("history.store.recovery_cut_follows_retained_prefix", "[component][history][recovery]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(22U);
    const auto slow_open = store.open(target, test_now);
    const auto fast_open = store.open(target, test_now);
    REQUIRE(slow_open.has_value());
    REQUIRE(fast_open.has_value());

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    const auto initial = std::make_unique<history_batch>(store.read(*fast_open, test_now));
    REQUIRE(initial->count == 1U);
    CHECK(initial->reset == history_reset::initial_open);

    cursor->enqueue_reading(make_sample(200U, 11U, 2U));
    cursor->enqueue_reading(make_sample(300U, 12U, 4U));
    cursor->enqueue_history_lost(history_loss::known, 3U, 91U);
    const auto failure_with_prefix =
        std::make_unique<history_batch>(store.read(*fast_open, test_now));
    REQUIRE(failure_with_prefix->count == 2U);
    if (failure_with_prefix->failure.has_value()) {
        CHECK(failure_with_prefix->failure->code == history_error::history_lost);
        CHECK(failure_with_prefix->failure->native_code == 91U);
    } else {
        FAIL("native history loss must be reported after the delivered prefix");
    }
    CHECK(failure_with_prefix->records[0U].sequence == 2U);
    CHECK(failure_with_prefix->records[1U].sequence == 3U);

    cursor->baseline = make_sample(400U, 13U, 8U);
    const auto recovery = std::make_unique<history_batch>(store.read(*fast_open, test_now));
    REQUIRE(recovery->count == 1U);
    CHECK(recovery->reset == history_reset::history_gap);
    CHECK(recovery->loss == history_loss::known);
    CHECK(recovery->lost_count == 3U);
    CHECK(recovery->records[0U].baseline);
    CHECK(recovery->records[0U].sample == cursor->baseline);
    CHECK(recovery->domain.stream != initial->domain.stream);

    const auto slow_initial = std::make_unique<history_batch>(store.read(*slow_open, test_now));
    REQUIRE(slow_initial->count == 1U);
    CHECK(slow_initial->records[0U].baseline);
    CHECK(slow_initial->reset == history_reset::initial_open);

    const auto retained_prefix = std::make_unique<history_batch>(store.read(*slow_open, test_now));
    REQUIRE(retained_prefix->count == 2U);
    CHECK(retained_prefix->records[0U].sample == make_sample(200U, 11U, 2U));
    CHECK(retained_prefix->records[1U].sample == make_sample(300U, 12U, 4U));
    CHECK(retained_prefix->records[0U].sequence == 2U);
    CHECK(retained_prefix->records[1U].sequence == 3U);
    CHECK(retained_prefix->reset == history_reset::none);

    const auto slow_recovery = std::make_unique<history_batch>(store.read(*slow_open, test_now));
    REQUIRE(slow_recovery->count == 1U);
    CHECK(slow_recovery->reset == history_reset::history_gap);
    CHECK(slow_recovery->loss == history_loss::known);
    CHECK(slow_recovery->lost_count == 3U);
    CHECK(slow_recovery->records[0U].sample == cursor->baseline);
    CHECK(slow_recovery->domain.stream != initial->domain.stream);
}

} // namespace inputnext::history_component_test
