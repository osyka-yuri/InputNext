#include "source_history_test_support.hpp"

#include "gameinput/history/history_capture.hpp"
#include "gameinput/history/history_capture_log.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include "support/history/history_capture_test_access.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;

struct pad_callback_run final {
    std::size_t count{};
    std::uint64_t first_qpc{};
    button_mask buttons{};
};

void append_pad_callbacks(history_capture& capture, pad_callback_run run) {
    for (std::size_t index = 0U; index < run.count; ++index) {
        const auto qpc = run.first_qpc + static_cast<std::uint64_t>(index);
        capture.on_callback(reading_parts{
            .pad = gamepad_state{.buttons = run.buttons},
            .source_timestamp_us = qpc,
            .observation_qpc = qpc,
        });
    }
}

TEST_CASE("gameinput.history.source_cursor_turns_seed_cuts_into_explicit_loss",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    SECTION("first guide seed cuts an active native cursor") {
        cursor_fixture fixture;
        fixture.capture->set_callback_coverage(true);
        REQUIRE(fixture.cursor.start(history_time{}).has_value());
        fixture.capture->on_seed(reading_parts{
            .guide = false,
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
        });

        CHECK(fixture.cursor.next(history_time{}).kind ==
              history_detail::cursor_step_kind::history_lost);
    }

    SECTION("changed pad seed cuts an active fallback cursor") {
        const auto zero = history_time{};
        cursor_fixture fixture;
        fixture.qualify_guide();
        fixture.capture->on_seed(reading_parts{
            .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
        });
        fixture.native.create_failures = 1U;
        REQUIRE(fixture.cursor.start(zero).has_value());

        fixture.capture->on_seed(reading_parts{
            .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
            .source_timestamp_us = 200U,
            .observation_qpc = 200U,
        });

        CHECK(fixture.cursor.next(zero + 100ms).kind ==
              history_detail::cursor_step_kind::history_lost);
    }
}

TEST_CASE("gameinput.history.source_cursor_drains_captured_prefix_before_exhaustion",
    "[component][gameinput][history][source]") {
    const auto zero = history_time{};
    scripted_now = zero;
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
        .source_timestamp_us = 100U,
        .observation_qpc = 100U,
    });
    fixture.native.create_failures = 1U;
    REQUIRE(fixture.cursor.start(zero).has_value());

    fixture.capture->on_callback(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 200U,
        .observation_qpc = 200U,
    });
    history_capture_test_access::exhaust_log(*fixture.capture);

    const auto prefix = fixture.cursor.next(zero + 100ms);
    REQUIRE(prefix.kind == history_detail::cursor_step_kind::reading);
    CHECK(prefix.reading.state.buttons == static_cast<button_mask>(gamepad_button::east));
    CHECK(fixture.cursor.next(zero + 100ms).kind == history_detail::cursor_step_kind::exhausted);
}

TEST_CASE("gameinput.history.source_cursor_does_not_replay_terminal_capture_after_start",
    "[component][gameinput][history][source]") {
    const auto zero = history_time{};
    scripted_now = zero;
    cursor_fixture fixture;
    history_capture_test_access::set_next_position(*fixture.capture,
        std::numeric_limits<std::uint64_t>::max());
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
        .source_timestamp_us = 100U,
        .observation_qpc = 100U,
    });
    REQUIRE(fixture.capture->snapshot().exhausted);

    fixture.native.create_failures = 1U;
    const auto seed = fixture.cursor.start(zero);
    REQUIRE(seed.has_value());
    CHECK(seed->baseline.state.buttons == static_cast<button_mask>(gamepad_button::south));
    CHECK(fixture.cursor.next(zero).kind == history_detail::cursor_step_kind::exhausted);
}

TEST_CASE("gameinput.history.source_cursor_falls_back_when_native_clock_seed_is_invalid",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    SECTION("valid fallback baseline and frequency") {
        cursor_fixture fixture;
        fixture.qualify_guide();
        fixture.capture->on_seed(reading_parts{
            .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
        });
        fixture.native.history.seed.qpc_frequency = 0U;

        const auto seed = fixture.cursor.start(history_time{});
        REQUIRE(seed.has_value());
        CHECK(seed->source == history_source_kind::callback_capture);
        CHECK(seed->qpc_frequency == fixture.native.frequency);
    }

    SECTION("no fallback baseline keeps clock failure pending") {
        cursor_fixture fixture;
        fixture.capture->set_callback_coverage(true);
        fixture.native.history.seed.qpc_frequency = 0U;

        const auto seed = fixture.cursor.start(history_time{});
        if (seed.has_value()) {
            FAIL("an invalid native clock without a fallback baseline must not start");
        } else {
            CHECK(seed.error().code == history_error::clock_discontinuity);
        }
    }
}

TEST_CASE("gameinput.history.source_cursor_skips_pad_capture_and_reports_guide_overwrite",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    SECTION("overwritten pad captures take one bounded progress step") {
        cursor_fixture fixture;
        fixture.qualify_guide();
        REQUIRE(fixture.cursor.start(history_time{}).has_value());

        for (std::size_t index = 0U; index < history_capture_capacity + 1U; ++index) {
            const auto value = static_cast<std::uint64_t>(index) + 101U;
            fixture.capture->on_callback(reading_parts{
                .pad = gamepad_state{.buttons = 1U},
                .source_timestamp_us = value,
                .observation_qpc = value,
            });
        }

        CHECK(
            fixture.cursor.next(history_time{}).kind == history_detail::cursor_step_kind::progress);
    }

    SECTION("overwritten guide control is explicit loss") {
        cursor_fixture fixture;
        fixture.qualify_guide();
        REQUIRE(fixture.cursor.start(history_time{}).has_value());

        fixture.capture->on_callback(reading_parts{
            .guide = true,
            .source_timestamp_us = 101U,
            .observation_qpc = 101U,
        });
        for (std::size_t index = 0U; index < history_capture_capacity; ++index) {
            const auto value = static_cast<std::uint64_t>(index) + 102U;
            fixture.capture->on_callback(reading_parts{
                .pad = gamepad_state{.buttons = 1U},
                .source_timestamp_us = value,
                .observation_qpc = value,
            });
        }

        CHECK(fixture.cursor.next(history_time{}).kind ==
              history_detail::cursor_step_kind::history_lost);
    }
}

TEST_CASE("gameinput.history.fallback_overwrite_with_cuts_and_restart_is_unknown_zero",
    "[component][gameinput][history][source]") {
    const auto zero = history_time{};
    scripted_now = zero;
    cursor_fixture fixture;
    fixture.native.create_failures = 2U;
    fixture.qualify_guide();
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
        .source_timestamp_us = 200U,
        .observation_qpc = 200U,
    });
    const auto seed = fixture.cursor.start(zero);
    REQUIRE(seed.has_value());
    CHECK(seed->source == history_source_kind::callback_capture);

    append_pad_callbacks(*fixture.capture,
        pad_callback_run{
            .count = 64U,
            .first_qpc = 300U,
            .buttons = static_cast<button_mask>(gamepad_button::south),
        });
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 500U,
        .observation_qpc = 500U,
    });
    append_pad_callbacks(*fixture.capture,
        pad_callback_run{
            .count = history_capture_capacity + 1U,
            .first_qpc = 600U,
            .buttons = static_cast<button_mask>(gamepad_button::south),
        });

    const auto overwritten = fixture.cursor.next(zero);
    CHECK(overwritten.kind == history_detail::cursor_step_kind::history_lost);
    CHECK(overwritten.loss == history_loss::unknown);
    CHECK(overwritten.lost_count == 0U);

    const auto restarted = fixture.cursor.start(zero);
    REQUIRE(restarted.has_value());
    CHECK(restarted->source == history_source_kind::callback_capture);
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 20'000U,
        .observation_qpc = 20'000U,
    });
    append_pad_callbacks(*fixture.capture,
        pad_callback_run{
            .count = history_capture_capacity + 1U,
            .first_qpc = 20'100U,
            .buttons = static_cast<button_mask>(gamepad_button::south),
        });

    const auto restarted_overwrite = fixture.cursor.next(zero);
    CHECK(restarted_overwrite.kind == history_detail::cursor_step_kind::history_lost);
    CHECK(restarted_overwrite.loss == history_loss::unknown);
    CHECK(restarted_overwrite.lost_count == 0U);
}

TEST_CASE("gameinput.history.source_cursor_fallback_freezes_horizon_and_promotes_at_fixed_cut",
    "[component][gameinput][history][source]") {
    const auto zero = history_time{};
    scripted_now = zero;
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.capture->on_seed(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
        .source_timestamp_us = 100U,
        .observation_qpc = 100U,
    });
    fixture.native.create_failures = 2U;

    const auto seed = fixture.cursor.start(zero);
    REQUIRE(seed.has_value());
    CHECK(seed->source == history_source_kind::callback_capture);
    CHECK(seed->baseline.effective_qpc == 100U);
    CHECK(fixture.cursor.next(zero + 500ms).checkpoint_qpc == 100U);
    CHECK(fixture.cursor.next(zero + 750ms).checkpoint_qpc == 100U);

    fixture.capture->on_callback(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 200U,
        .observation_qpc = 200U,
    });
    const auto pad = fixture.cursor.next(zero + 800ms);
    REQUIRE(pad.kind == history_detail::cursor_step_kind::reading);
    CHECK(pad.reading.effective_qpc == 200U);
    CHECK(fixture.cursor.next(zero + 800ms).checkpoint_qpc == 200U);

    scripted_now = zero + 3s;
    CHECK(fixture.cursor.next(zero + 1s).kind == history_detail::cursor_step_kind::progress);
    CHECK(fixture.native.create_count == 2U);

    fixture.native.create_failures = 0U;
    CHECK(fixture.cursor.next(zero + 3999ms).kind == history_detail::cursor_step_kind::caught_up);
    CHECK(fixture.native.create_count == 2U);

    fixture.native.history.quiet_checkpoint_qpc = 400U;
    CHECK(fixture.cursor.next(zero + 4s).kind == history_detail::cursor_step_kind::progress);
    CHECK(fixture.native.create_count == 3U);

    fixture.capture->on_callback(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::west)},
        .source_timestamp_us = 300U,
        .observation_qpc = 300U,
    });
    CHECK(fixture.cursor.next(zero + 4s).kind == history_detail::cursor_step_kind::history_lost);

    const auto restarted = fixture.cursor.start(zero + 4s);
    REQUIRE(restarted.has_value());
    CHECK(restarted->source == history_source_kind::native_history);
    CHECK(fixture.cursor.next(zero + 4s).kind == history_detail::cursor_step_kind::caught_up);
}

} // namespace
} // namespace inputnext::gameinput_detail
