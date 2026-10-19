#include "ring.hpp"

#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <memory>

namespace inputnext::history_detail {

struct history_ring_test_access final {
    static void set_empty_position(history_ring& ring,
        std::uint64_t position,
        ring_loss_mark mark = {}) noexcept {
        ring.oldest_position_ = position;
        ring.next_position_ = position;
        ring.tail_loss_mark_ = mark;
        ring.latest_unknown_cut_ = 0U;
    }
};

} // namespace inputnext::history_detail

namespace {

using namespace inputnext;
using namespace inputnext::history_detail;

[[nodiscard]] ring_entry entry(ring_entry_kind kind = ring_entry_kind::reading,
    history_loss loss = history_loss::none,
    std::uint64_t lost_count = 0U,
    history_record_origin origin = history_record_origin::native_pad) {
    return ring_entry{
        .sample = history_sample{.state = gamepad_state{.buttons = 1U},
            .source_timestamp_us = 10U,
            .effective_qpc = 20U},
        .source = history_source_kind::native_history,
        .kind = kind,
        .loss = loss,
        .lost_count = lost_count,
        .origin = origin,
    };
}

TEST_CASE("history.ring.retains_newest_bounded_window", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};

    for (std::uint64_t value = 0U; value < history_bank_capacity + 1U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
        REQUIRE(appended_position == value + 1U);
    }

    CHECK(ring->oldest_position() == 2U);
    CHECK(ring->next_position() == history_bank_capacity + 2U);
    CHECK(ring->find(1U) == nullptr);
    REQUIRE(ring->find(2U) != nullptr);
    REQUIRE(ring->latest() != nullptr);
    CHECK(ring->latest()->position == history_bank_capacity + 1U);
    CHECK(ring->overrun(1U, {}).loss == history_loss::known);
    CHECK(ring->overrun(1U, {}).lost_count == history_bank_capacity);
}

TEST_CASE("history.ring.marks_evicted_cut_as_unknown_loss", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};

    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::unknown),
        appended_position));
    for (std::uint64_t value = 0U; value < history_bank_capacity; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }

    CHECK(ring->oldest_position() == 2U);
    CHECK(ring->overrun(1U, {}).loss == history_loss::unknown);
    CHECK(ring->overrun(1U, {}).lost_count == 0U);
    CHECK(ring->overrun(2U, {}).loss == history_loss::none);
}

TEST_CASE("history.ring.counts_multiple_known_recovery_cuts_and_excludes_latest_reading",
    "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};
    REQUIRE(ring->append(entry(ring_entry_kind::initial_baseline), appended_position));

    for (std::uint64_t value = 0U; value < 100U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }
    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::known, 3U),
        appended_position));
    for (std::uint64_t value = 0U; value < 100U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }
    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::known, 4U),
        appended_position));
    for (std::uint64_t value = 0U; value < 54U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }

    CHECK(ring->oldest_position() == 2U);
    CHECK(ring->overrun(1U, {}).loss == history_loss::known);
    CHECK(ring->overrun(1U, {}).lost_count == 260U);
}

TEST_CASE("history.ring.includes_loss_from_latest_recovery_boundary", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};
    REQUIRE(ring->append(entry(ring_entry_kind::initial_baseline), appended_position));
    for (std::uint64_t value = 0U; value < history_bank_capacity - 1U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }
    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::known, 9U),
        appended_position));

    const auto overrun = ring->overrun(1U, {});
    CHECK(overrun.loss == history_loss::known);
    CHECK(overrun.lost_count == history_bank_capacity - 1U + 9U);
}

TEST_CASE("history.ring.does_not_taint_interval_with_consumed_unknown_cut", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};
    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::unknown),
        appended_position));
    for (std::uint64_t value = 0U; value < history_bank_capacity + 1U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }

    const auto overrun = ring->overrun(2U, {});
    CHECK(overrun.loss == history_loss::known);
    CHECK(overrun.lost_count == history_bank_capacity);
}

TEST_CASE("history.ring.unknown_retained_boundary_marks_overrun_unknown", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};
    for (std::uint64_t value = 0U; value < 200U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }
    REQUIRE(ring->append(entry(ring_entry_kind::recovery_baseline, history_loss::unknown),
        appended_position));
    for (std::uint64_t value = 0U; value < 56U; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }

    CHECK(ring->oldest_position() == 2U);
    CHECK(ring->overrun(1U, {}).loss == history_loss::unknown);
    CHECK(ring->overrun(1U, {}).lost_count == 0U);
}

TEST_CASE("history.ring.ordinary_prefix_overflow_becomes_unknown_cut", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    auto appended_position = std::uint64_t{};
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    history_ring_test_access::set_empty_position(*ring, 1U, ring_loss_mark{maximum});
    REQUIRE(ring->append(entry(), appended_position));
    for (std::uint64_t value = 0U; value < history_bank_capacity; ++value) {
        REQUIRE(ring->append(entry(), appended_position));
    }

    const auto overrun = ring->overrun(1U, ring_loss_mark{maximum});
    CHECK(overrun.loss == history_loss::unknown);
    CHECK(overrun.lost_count == 0U);
}

TEST_CASE("history.ring.refuses_position_wrap", "[unit][history]") {
    const auto ring = std::make_unique<history_ring>();
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    history_ring_test_access::set_empty_position(*ring, maximum - 1U, ring_loss_mark{41U});
    auto appended_position = std::uint64_t{};

    REQUIRE(ring->append(entry(), appended_position));
    CHECK(appended_position == maximum - 1U);
    CHECK(ring->next_position() == maximum);
    CHECK(ring->tail_loss_mark() == ring_loss_mark{42U});
    CHECK(!ring->append(entry(), appended_position));
    CHECK(ring->tail_loss_mark() == ring_loss_mark{42U});
    CHECK(ring->oldest_position() == maximum - 1U);
    CHECK(ring->next_position() == maximum);
}

} // namespace
