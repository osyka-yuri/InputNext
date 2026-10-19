#include "history_store_test_support.hpp"

#include "ring.hpp"
#include "state.hpp"
#include "support/history/history_store_test_access.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace inputnext::history_component_test {

TEST_CASE("history.store.open_remains_pending_until_baseline", "[component][history]") {
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(1U);
    const auto opened = store.open(target, test_now);
    REQUIRE(opened.has_value());
    const auto handle = *opened;
    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    cursor->fail_start(history_error::no_data);

    const auto pending = std::make_unique<history_batch>(store.read(handle, test_now));
    CHECK(pending->count == 0U);
    if (pending->failure.has_value()) {
        CHECK(pending->failure->code == history_error::no_data);
    } else {
        FAIL("the initial cursor start must report no data");
    }
    const auto expected_domain = history_domain{.target = target, .stream = 1U};
    CHECK(pending->domain == expected_domain);

    const auto baseline = std::make_unique<history_batch>(store.read(handle, test_now));
    REQUIRE(baseline->count == 1U);
    CHECK(!baseline->failure.has_value());
    CHECK(baseline->reset == history_reset::initial_open);
    CHECK(baseline->records[0U].baseline);
    CHECK(baseline->records[0U].origin == history_record_origin::baseline);
    CHECK(baseline->records[0U].sequence == 1U);
    CHECK(baseline->records[0U].sample == cursor->baseline);
    CHECK(baseline->source == history_source_kind::native_history);

    cursor->enqueue_reading(make_sample(400U, 11U, 2U));
    cursor->enqueue_reading(make_sample(300U, 12U, 4U));
    cursor->enqueue_provider_failure(17U);
    const auto prefix = std::make_unique<history_batch>(store.read(handle, test_now));
    REQUIRE(prefix->count == 2U);
    CHECK(prefix->records[0U].sequence == 2U);
    CHECK(prefix->records[1U].sequence == 3U);
    CHECK(prefix->records[0U].sample.source_timestamp_us == 400U);
    CHECK(prefix->records[1U].sample.source_timestamp_us == 300U);
    CHECK(prefix->records[0U].origin == history_record_origin::native_pad);
    CHECK(prefix->records[1U].origin == history_record_origin::native_pad);
    if (prefix->failure.has_value()) {
        CHECK(prefix->failure->code == history_error::provider_failure);
        CHECK(prefix->failure->native_code == 17U);
    } else {
        FAIL("the provider failure must follow the delivered prefix");
    }

    cursor->enqueue_reading(make_sample(250U, 13U, 8U));
    const auto resumed = std::make_unique<history_batch>(store.read(handle, test_now));
    REQUIRE(resumed->count == 1U);
    CHECK(resumed->records[0U].sequence == 4U);
    CHECK(resumed->records[0U].sample.source_timestamp_us == 250U);
    CHECK(resumed->records[0U].origin == history_record_origin::native_pad);
    CHECK(resumed->caught_up);
    CHECK(resumed->checkpoint_qpc == 13U);
    CHECK(resumed->checkpoint_sequence == 4U);
}

TEST_CASE("history.store.readers_share_bank_with_independent_streams", "[component][history]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(2U);
    const auto first_open = store.open(target, test_now);
    REQUIRE(first_open.has_value());
    const auto first = *first_open;
    const auto first_baseline = std::make_unique<history_batch>(store.read(first, test_now));
    REQUIRE(first_baseline->count == 1U);

    const auto second_open = store.open(target, test_now);
    REQUIRE(second_open.has_value());
    const auto second = *second_open;
    const auto second_baseline = std::make_unique<history_batch>(store.read(second, test_now));
    REQUIRE(second_baseline->count == 1U);
    CHECK(second_baseline->records[0U].sample == first_baseline->records[0U].sample);
    CHECK(second_baseline->reset == history_reset::initial_open);
    CHECK(second_baseline->domain.stream != first_baseline->domain.stream);
    CHECK(provider.factory_calls.load(std::memory_order_relaxed) == 1U);

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    cursor->enqueue_reading(make_sample(500U, 11U, 2U), history_record_origin::native_pad);
    cursor->enqueue_reading(make_sample(500U, 12U, 4U), history_record_origin::callback_guide);

    const auto first_events = std::make_unique<history_batch>(store.read(first, test_now));
    const auto second_events = std::make_unique<history_batch>(store.read(second, test_now));
    REQUIRE(first_events->count == 2U);
    REQUIRE(second_events->count == 2U);
    CHECK(first_events->records[0U].sequence == 2U);
    CHECK(first_events->records[1U].sequence == 3U);
    CHECK(second_events->records[0U].sequence == 2U);
    CHECK(second_events->records[1U].sequence == 3U);
    CHECK(first_events->records[0U].sample.source_timestamp_us ==
          first_events->records[1U].sample.source_timestamp_us);
    CHECK(first_events->records[0U].origin == history_record_origin::native_pad);
    CHECK(first_events->records[1U].origin == history_record_origin::callback_guide);
    CHECK(second_events->records[0U].origin == history_record_origin::native_pad);
    CHECK(second_events->records[1U].origin == history_record_origin::callback_guide);
    CHECK(first_events->records[0U].sample.effective_qpc <
          first_events->records[1U].sample.effective_qpc);
    CHECK(first_events->domain.stream == first_baseline->domain.stream);
    CHECK(second_events->domain.stream == second_baseline->domain.stream);
}

TEST_CASE("history.store.overrun_resets_only_the_slow_reader", "[component][history]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(3U);
    const auto slow_open = store.open(target, test_now);
    const auto fast_open = store.open(target, test_now);
    REQUIRE(slow_open.has_value());
    REQUIRE(fast_open.has_value());
    const auto slow = *slow_open;
    const auto fast = *fast_open;

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    const auto baseline = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(baseline->count == 1U);

    for (std::uint64_t index = 0U; index < history_bank_capacity + 1U; ++index) {
        const auto qpc = 11U + index;
        cursor->enqueue_reading(
            make_sample(1'000U + index, qpc, static_cast<button_mask>(1U << (index % 14U))));
    }

    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto batch = std::make_unique<history_batch>(store.read(fast, test_now));
        REQUIRE(batch->count == history_batch_capacity);
        CHECK(!batch->failure.has_value());
    }
    const auto final_fast_batch = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(final_fast_batch->count == 1U);
    CHECK(final_fast_batch->caught_up);

    const auto slow_reset = std::make_unique<history_batch>(store.read(slow, test_now));
    REQUIRE(slow_reset->count == 1U);
    CHECK(slow_reset->reset == history_reset::history_gap);
    CHECK(slow_reset->loss == history_loss::known);
    CHECK(slow_reset->lost_count == history_bank_capacity);
    CHECK(slow_reset->records[0U].baseline);
    CHECK(slow_reset->records[0U].origin == history_record_origin::baseline);
    CHECK(slow_reset->records[0U].sequence == 1U);
    CHECK(slow_reset->domain.stream != baseline->domain.stream);
    CHECK(slow_reset->records[0U].sample ==
          make_sample(1'000U + history_bank_capacity,
              11U + history_bank_capacity,
              static_cast<button_mask>(1U << (history_bank_capacity % 14U))));

    const auto fast_caught_up = std::make_unique<history_batch>(store.read(fast, test_now));
    CHECK(fast_caught_up->domain.stream == baseline->domain.stream);
    CHECK(!fast_caught_up->failure.has_value());
}

TEST_CASE("history.store.overrun_uses_selected_open_loss_mark_and_is_reader_local",
    "[component][history][recovery]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(30U);
    const auto fast_open = store.open(target, test_now);
    REQUIRE(fast_open.has_value());
    const auto fast = *fast_open;

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    const auto initial = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(initial->count == 1U);
    CHECK(initial->records[0U].origin == history_record_origin::baseline);

    for (std::uint64_t index = 0U; index < 5U; ++index) {
        cursor->enqueue_reading(make_sample(200U + index, 11U + index));
    }
    cursor->enqueue_history_lost(history_loss::known, 7U);
    const auto prefix = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(prefix->count == 5U);
    if (prefix->failure.has_value()) {
        CHECK(prefix->failure->code == history_error::history_lost);
    } else {
        FAIL("the provider loss must follow the delivered prefix");
    }

    cursor->baseline = make_sample(300U, 16U);
    const auto recovery = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(recovery->count == 1U);
    CHECK(recovery->reset == history_reset::history_gap);
    CHECK(recovery->loss == history_loss::known);
    CHECK(recovery->lost_count == 7U);
    CHECK(recovery->records[0U].origin == history_record_origin::baseline);

    const auto later_open = store.open(target, test_now);
    REQUIRE(later_open.has_value());
    const auto later = *later_open;

    for (std::uint64_t index = 0U; index < history_bank_capacity + 1U; ++index) {
        cursor->enqueue_reading(make_sample(1'000U + index, 17U + index));
    }
    for (std::size_t index = 0U; index < history_bank_capacity / history_batch_capacity; ++index) {
        const auto batch = std::make_unique<history_batch>(store.read(fast, test_now));
        REQUIRE(batch->count == history_batch_capacity);
        CHECK(!batch->failure.has_value());
    }
    const auto final_fast_batch = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(final_fast_batch->count == 1U);
    CHECK(final_fast_batch->records[0U].origin == history_record_origin::native_pad);

    const auto selected_open_baseline =
        std::make_unique<history_batch>(store.read(later, test_now));
    REQUIRE(selected_open_baseline->count == 1U);
    CHECK(selected_open_baseline->reset == history_reset::initial_open);
    CHECK(selected_open_baseline->records[0U].baseline);
    CHECK(selected_open_baseline->records[0U].origin == history_record_origin::baseline);

    const auto later_reset = std::make_unique<history_batch>(store.read(later, test_now));
    REQUIRE(later_reset->count == 1U);
    CHECK(later_reset->reset == history_reset::history_gap);
    CHECK(later_reset->loss == history_loss::known);
    CHECK(later_reset->lost_count == history_bank_capacity);
    CHECK(later_reset->records[0U].sample.source_timestamp_us == 1'000U + history_bank_capacity);
    CHECK(later_reset->records[0U].origin == history_record_origin::baseline);

    const auto fast_continues = std::make_unique<history_batch>(store.read(fast, test_now));
    CHECK(fast_continues->domain.stream == recovery->domain.stream);
    CHECK(!fast_continues->failure.has_value());
}

TEST_CASE("history.store.retained_unknown_recovery_cut_resets_slow_reader_as_unknown",
    "[component][history][recovery]") {
    auto provider = fake_history_provider{};
    const auto store_storage = std::make_unique<history_store>(provider);
    auto& store = *store_storage;
    const auto target = make_target(32U);
    const auto slow_open = store.open(target, test_now);
    const auto fast_open = store.open(target, test_now);
    REQUIRE(slow_open.has_value());
    REQUIRE(fast_open.has_value());
    const auto slow = *slow_open;
    const auto fast = *fast_open;

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    const auto fast_baseline = std::make_unique<history_batch>(store.read(fast, test_now));
    const auto slow_baseline = std::make_unique<history_batch>(store.read(slow, test_now));
    REQUIRE(fast_baseline->count == 1U);
    REQUIRE(slow_baseline->count == 1U);
    CHECK(fast_baseline->records[0U].baseline);
    CHECK(slow_baseline->records[0U].baseline);

    for (std::uint64_t index = 0U; index < 130U; ++index) {
        cursor->enqueue_reading(make_sample(200U + index, 11U + index));
    }
    cursor->enqueue_history_lost(history_loss::unknown, 0U);
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto batch = std::make_unique<history_batch>(store.read(fast, test_now));
        REQUIRE(batch->count == history_batch_capacity);
        CHECK(!batch->failure.has_value());
    }
    const auto unknown_cut_prefix = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(unknown_cut_prefix->count == 2U);
    if (unknown_cut_prefix->failure.has_value()) {
        CHECK(unknown_cut_prefix->failure->code == history_error::history_lost);
    } else {
        FAIL("the unknown source loss must follow the delivered prefix");
    }

    cursor->baseline = make_sample(500U, 141U);
    const auto recovery = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(recovery->count == 1U);
    CHECK(recovery->reset == history_reset::history_gap);
    CHECK(recovery->loss == history_loss::unknown);
    CHECK(recovery->lost_count == 0U);
    CHECK(recovery->records[0U].baseline);
    CHECK(recovery->records[0U].sample == cursor->baseline);

    for (std::uint64_t index = 0U; index < 126U; ++index) {
        cursor->enqueue_reading(make_sample(1'000U + index, 142U + index));
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
        const auto batch = std::make_unique<history_batch>(store.read(fast, test_now));
        REQUIRE(batch->count == history_batch_capacity);
        CHECK(!batch->failure.has_value());
    }
    const auto final_fast_batch = std::make_unique<history_batch>(store.read(fast, test_now));
    REQUIRE(final_fast_batch->count == 30U);
    CHECK(!final_fast_batch->failure.has_value());

    const auto slow_reset = std::make_unique<history_batch>(store.read(slow, test_now));
    REQUIRE(slow_reset->count == 1U);
    CHECK(slow_reset->reset == history_reset::history_gap);
    CHECK(slow_reset->loss == history_loss::unknown);
    CHECK(slow_reset->lost_count == 0U);
    CHECK(slow_reset->records[0U].baseline);
    CHECK(slow_reset->records[0U].origin == history_record_origin::baseline);
    CHECK(slow_reset->records[0U].sample.source_timestamp_us == 1'000U + 125U);
    CHECK(slow_reset->domain.stream != slow_baseline->domain.stream);

    const auto fast_continues = std::make_unique<history_batch>(store.read(fast, test_now));
    CHECK(fast_continues->domain.stream == recovery->domain.stream);
    CHECK(fast_continues->caught_up);
    CHECK(!fast_continues->failure.has_value());
}

TEST_CASE("history.store.stream_identity_exhaustion_preserves_other_reader",
    "[component][history]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto provider = fake_history_provider{};
    auto store =
        history_store{provider, identity_seed{.next_reader = 1U, .next_stream = maximum - 1U}};
    const auto target = make_target(4U);
    const auto slow_open = store.open(target, test_now);
    const auto fast_open = store.open(target, test_now);
    REQUIRE(slow_open.has_value());
    REQUIRE(fast_open.has_value());
    const auto slow = *slow_open;
    const auto fast = *fast_open;
    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    const auto baseline = store.read(fast, test_now);
    REQUIRE(baseline.count == 1U);

    for (std::uint64_t index = 0U; index < history_bank_capacity + 1U; ++index) {
        cursor->enqueue_reading(make_sample(2'000U + index, 11U + index));
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
        REQUIRE(store.read(fast, test_now).count == history_batch_capacity);
    }
    REQUIRE(store.read(fast, test_now).count == 1U);

    const auto failed_reset = store.read(slow, test_now);
    CHECK(failed_reset.count == 0U);
    CHECK(failed_reset.domain.stream == maximum - 1U);
    if (failed_reset.failure.has_value()) {
        CHECK(failed_reset.failure->code == history_error::identity_exhausted);
    } else {
        FAIL("an exhausted stream identity must be reported");
    }

    const auto fast_continues = store.read(fast, test_now);
    CHECK(fast_continues.domain.stream == maximum);
    CHECK(!fast_continues.failure.has_value());
}

TEST_CASE("history.store.output_sequence_exhaustion_is_reader_local", "[component][history]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto target = make_target(5U);
    const auto first_open = store.open(target, test_now);
    const auto second_open = store.open(target, test_now);
    REQUIRE(first_open.has_value());
    REQUIRE(second_open.has_value());
    const auto first = *first_open;
    const auto second = *second_open;
    REQUIRE(store.read(first, test_now).count == 1U);
    REQUIRE(store.read(second, test_now).count == 1U);

    const auto cursor = provider.cursor_state(target);
    REQUIRE(cursor != nullptr);
    cursor->enqueue_reading(make_sample(600U, 11U, 2U));
    history_store_test_access::set_next_output_sequence(store, first, maximum);

    const auto exhausted = store.read(first, test_now);
    REQUIRE(exhausted.count == 1U);
    CHECK(exhausted.records[0U].sequence == maximum);
    if (exhausted.failure.has_value()) {
        CHECK(exhausted.failure->code == history_error::sequence_exhausted);
    } else {
        FAIL("the exhausted reader must report its terminal sequence failure");
    }

    const auto other_reader = store.read(second, test_now);
    REQUIRE(other_reader.count == 1U);
    CHECK(other_reader.records[0U].sequence == 2U);
    CHECK(other_reader.records[0U].sample.source_timestamp_us == 600U);
    CHECK(!other_reader.failure.has_value());
    CHECK(store.close(first).has_value());
}

TEST_CASE("history.store.bank_position_exhaustion_is_bank_local", "[component][history]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto provider = fake_history_provider{};
    auto store = history_store{provider};
    const auto exhausted_target = make_target(6U);
    const auto active_target = make_target(7U);
    const auto exhausted_open = store.open(exhausted_target, test_now);
    const auto active_open = store.open(active_target, test_now);
    REQUIRE(exhausted_open.has_value());
    REQUIRE(active_open.has_value());
    const auto exhausted_reader = *exhausted_open;
    const auto active_reader = *active_open;
    REQUIRE(store.read(exhausted_reader, test_now).count == 1U);
    REQUIRE(store.read(active_reader, test_now).count == 1U);

    history_store_test_access::set_ring_tail(store, exhausted_reader, maximum);
    const auto exhausted = store.read(exhausted_reader, test_now);
    CHECK(exhausted.count == 0U);
    if (exhausted.failure.has_value()) {
        CHECK(exhausted.failure->code == history_error::sequence_exhausted);
    } else {
        FAIL("the exhausted bank must report its terminal position failure");
    }

    const auto active_cursor = provider.cursor_state(active_target);
    REQUIRE(active_cursor != nullptr);
    active_cursor->enqueue_reading(make_sample(700U, 11U, 2U));
    const auto active_batch = store.read(active_reader, test_now);
    REQUIRE(active_batch.count == 1U);
    CHECK(active_batch.records[0U].sequence == 2U);
    CHECK(active_batch.records[0U].sample.source_timestamp_us == 700U);
    CHECK(!active_batch.failure.has_value());

    REQUIRE(store.close(exhausted_reader).has_value());
    store.retire(exhausted_target);
    CHECK(store.drain_retired() == 1U);

    active_cursor->enqueue_reading(make_sample(800U, 12U, 4U));
    const auto active_after_drain = store.read(active_reader, test_now);
    REQUIRE(active_after_drain.count == 1U);
    CHECK(active_after_drain.records[0U].sequence == 3U);
    CHECK(active_after_drain.records[0U].sample.source_timestamp_us == 800U);
    CHECK(!active_after_drain.failure.has_value());
}

} // namespace inputnext::history_component_test
