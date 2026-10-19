#include "gameinput/history/history_capture.hpp"

#include "gameinput/history/history_capture_log.hpp"
#include "gameinput/reading_parts.hpp"
#include "support/history/history_capture_test_access.hpp"
#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace inputnext::gameinput_detail {

namespace {

[[nodiscard]] reading_parts
pad_seed(button_mask buttons, std::uint64_t timestamp, std::uint64_t qpc) {
    return reading_parts{
        .pad = gamepad_state{.buttons = buttons},
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
    };
}

[[nodiscard]] reading_parts guide_seed(bool pressed, std::uint64_t timestamp, std::uint64_t qpc) {
    return reading_parts{
        .guide = pressed,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
    };
}

[[nodiscard]] reading_parts
pad_callback(button_mask buttons, std::uint64_t timestamp, std::uint64_t qpc) {
    return pad_seed(buttons, timestamp, qpc);
}

[[nodiscard]] reading_parts
guide_callback(bool pressed, std::uint64_t timestamp, std::uint64_t qpc) {
    return guide_seed(pressed, timestamp, qpc);
}

TEST_CASE("gameinput.history.capture_requires_fresh_guide_after_coverage_cut",
    "[component][gameinput][history][capture]") {
    const auto capture = std::make_unique<history_capture>();

    const auto initial = capture->snapshot();
    CHECK_FALSE(initial.callback_coverage);
    CHECK_FALSE(initial.guide_fresh);

    capture->on_seed(pad_seed(1U, 100U, 1000U));
    capture->on_seed(guide_seed(false, 100U, 1000U));
    CHECK(capture->snapshot().guide_fresh);

    capture->set_callback_coverage(true);
    const auto activated = capture->snapshot();
    CHECK(activated.callback_coverage);
    CHECK_FALSE(activated.guide_fresh);
    CHECK(activated.coverage_generation == 1U);
    CHECK(activated.next_position == 4U);
    CHECK(capture->read(3U).record.kind == capture_record_kind::guide_cut);

    capture->set_callback_coverage(true);
    CHECK(capture->snapshot().next_position == activated.next_position);

    capture->on_callback(guide_callback(true, 120U, 1200U));
    const auto refreshed = capture->snapshot();
    CHECK(refreshed.guide_fresh);
    CHECK(refreshed.next_position == activated.next_position + 1U);
    const auto guide = capture->read(activated.next_position);
    REQUIRE(guide.kind == capture_read_kind::record);
    CHECK(guide.record.kind == capture_record_kind::guide);
    CHECK(guide.record.sample.guide_known);
    CHECK((guide.record.sample.state.buttons & static_cast<button_mask>(gamepad_button::guide)) !=
          0U);

    capture->set_callback_coverage(false);
    const auto deactivated = capture->snapshot();
    CHECK_FALSE(deactivated.callback_coverage);
    CHECK_FALSE(deactivated.guide_fresh);
    CHECK(deactivated.coverage_generation == 2U);
    CHECK(capture->read(refreshed.next_position).record.kind == capture_record_kind::guide_cut);
}

TEST_CASE("gameinput.history.capture_seeds_create_only_changed_state_cuts",
    "[component][gameinput][history][capture]") {
    const auto capture = std::make_unique<history_capture>();
    capture->on_seed(pad_seed(1U, 100U, 1000U));
    const auto snapshot = capture->snapshot();
    REQUIRE(snapshot.fallback_baseline.has_value());
    const auto baseline = snapshot.fallback_baseline.value_or(history_sample{});
    CHECK_FALSE(baseline.guide_known);
    CHECK(capture->read(1U).record.kind == capture_record_kind::fallback_cut);

    capture->on_seed(pad_seed(1U, 100U, 1100U));
    CHECK(capture->snapshot().next_position == 2U);

    capture->on_seed(pad_seed(1U, 101U, 1200U));
    CHECK(capture->snapshot().next_position == 3U);
    CHECK(capture->read(2U).record.kind == capture_record_kind::fallback_cut);

    capture->on_seed(guide_seed(false, 101U, 1200U));
    CHECK(capture->snapshot().guide_fresh);
    CHECK(capture->snapshot().next_position == 4U);
    CHECK(capture->read(3U).record.kind == capture_record_kind::guide_cut);

    capture->on_seed(guide_seed(false, 102U, 1300U));
    CHECK(capture->snapshot().next_position == 4U);

    capture->on_seed(guide_seed(true, 103U, 1400U));
    CHECK(capture->snapshot().next_position == 5U);
    CHECK(capture->read(4U).record.kind == capture_record_kind::guide_cut);
}

TEST_CASE("gameinput.history.capture_overwrite_distinguishes_pad_from_guide_loss",
    "[component][gameinput][history][capture]") {
    const auto capture = std::make_unique<history_capture>();
    capture->set_callback_coverage(true);

    for (std::size_t index = 0U; index < history_capture_capacity + 1U; ++index) {
        const auto timestamp = static_cast<std::uint64_t>(index) + 1U;
        capture->on_callback(pad_callback(1U, timestamp, timestamp));
    }

    const auto pad_overwrite = capture->read(1U);
    CHECK(pad_overwrite.kind == capture_read_kind::overwritten);
    CHECK(pad_overwrite.overwritten_control_position == 1U);

    capture->on_callback(guide_callback(false, 300U, 300U));
    for (std::size_t index = 0U; index < history_capture_capacity; ++index) {
        const auto timestamp = static_cast<std::uint64_t>(index) + 301U;
        capture->on_callback(pad_callback(1U, timestamp, timestamp));
    }

    const auto guide_overwrite = capture->snapshot();
    CHECK(guide_overwrite.overwritten_control_position == history_capture_capacity + 3U);
    CHECK(
        capture->read(guide_overwrite.oldest_position - 1U).kind == capture_read_kind::overwritten);
}

TEST_CASE("gameinput.history.capture_log_exhaustion_is_terminal_and_bounded",
    "[component][gameinput][history][capture]") {
    const auto log = std::make_unique<history_capture_log>();
    capture_log_test_access::set_next_position(*log, std::numeric_limits<std::uint64_t>::max());

    log->append(capture_record_kind::pad,
        history_sample{.state = gamepad_state{.buttons = 1U},
            .source_timestamp_us = 1U,
            .effective_qpc = 1U});

    CHECK(log->exhausted());
    CHECK(log->next_position() == std::numeric_limits<std::uint64_t>::max());
    CHECK(log->read(std::numeric_limits<std::uint64_t>::max()).kind == capture_read_kind::record);
    CHECK(log->read(std::numeric_limits<std::uint64_t>::max() - 1U).kind ==
          capture_read_kind::overwritten);
    log->append(capture_record_kind::pad, history_sample{});
    CHECK(log->tail_position() == std::numeric_limits<std::uint64_t>::max());
}

} // namespace

} // namespace inputnext::gameinput_detail
