#include "source_history_test_support.hpp"

#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE("gameinput.history.source_cursor_orders_pad_tie_release_before_checkpoint",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide(true);
    fixture.native.history.seed.baseline.state.buttons =
        static_cast<button_mask>(gamepad_button::south);
    fixture.native.history.steps[0U] =
        reading_step(static_cast<button_mask>(gamepad_button::east), 200U, 200U);
    fixture.native.history.steps[1U] = caught_up_step(200U);
    fixture.native.history.step_count = 2U;

    const auto seed = fixture.cursor.start(history_time{});
    REQUIRE(seed.has_value());
    fixture.capture->on_callback(reading_parts{
        .guide = false,
        .source_timestamp_us = 50U,
        .observation_qpc = 200U,
    });

    const auto pad = fixture.cursor.next(history_time{});
    REQUIRE(pad.kind == history_detail::cursor_step_kind::reading);
    CHECK(pad.reading.effective_qpc == 200U);
    CHECK(pad.reading.source_timestamp_us == 200U);
    CHECK(
        pad.reading.state.buttons == (static_cast<button_mask>(gamepad_button::east) | guide_mask));

    const auto guide = fixture.cursor.next(history_time{});
    REQUIRE(guide.kind == history_detail::cursor_step_kind::reading);
    CHECK(guide.reading.effective_qpc == 200U);
    CHECK(guide.reading.source_timestamp_us == 50U);
    CHECK(guide.reading.state.buttons == static_cast<button_mask>(gamepad_button::east));

    const auto quiet = fixture.cursor.next(history_time{});
    CHECK(quiet.kind == history_detail::cursor_step_kind::caught_up);
    CHECK(quiet.checkpoint_qpc == 200U);
}

TEST_CASE("gameinput.history.source_cursor_readers_progress_independently_on_shared_capture",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide(true);
    fixture.native.history.seed.baseline.state.buttons =
        static_cast<button_mask>(gamepad_button::south);
    fixture.native.history.steps[0U] =
        reading_step(static_cast<button_mask>(gamepad_button::east), 200U, 200U);
    fixture.native.history.steps[1U] = caught_up_step(200U);
    fixture.native.history.step_count = 2U;

    source_history_cursor second_cursor{
        fixture.native,
        native_device_handle{&fixture.device},
        *fixture.capture,
    };
    source_history_cursor_test_access::use_scripted_clock(second_cursor);

    REQUIRE(fixture.cursor.start(history_time{}).has_value());
    REQUIRE(second_cursor.start(history_time{}).has_value());
    fixture.capture->on_callback(reading_parts{
        .guide = false,
        .source_timestamp_us = 50U,
        .observation_qpc = 200U,
    });

    const auto first_pad = fixture.cursor.next(history_time{});
    const auto second_pad = second_cursor.next(history_time{});
    REQUIRE(first_pad.kind == history_detail::cursor_step_kind::reading);
    REQUIRE(second_pad.kind == history_detail::cursor_step_kind::reading);
    const auto pad_state = static_cast<button_mask>(gamepad_button::east) | guide_mask;
    CHECK(first_pad.reading.state.buttons == pad_state);
    CHECK(second_pad.reading.state.buttons == pad_state);
    CHECK(first_pad.reading.effective_qpc == 200U);
    CHECK(second_pad.reading.effective_qpc == 200U);

    const auto first_guide = fixture.cursor.next(history_time{});
    const auto second_guide = second_cursor.next(history_time{});
    REQUIRE(first_guide.kind == history_detail::cursor_step_kind::reading);
    REQUIRE(second_guide.kind == history_detail::cursor_step_kind::reading);
    const auto released_pad_state = static_cast<button_mask>(gamepad_button::east);
    CHECK(first_guide.reading.state.buttons == released_pad_state);
    CHECK(second_guide.reading.state.buttons == released_pad_state);
    CHECK(first_guide.reading.source_timestamp_us == 50U);
    CHECK(second_guide.reading.source_timestamp_us == 50U);
    CHECK(first_guide.reading.effective_qpc == 200U);
    CHECK(second_guide.reading.effective_qpc == 200U);

    const auto first_checkpoint = fixture.cursor.next(history_time{});
    const auto second_checkpoint = second_cursor.next(history_time{});
    CHECK(first_checkpoint.kind == history_detail::cursor_step_kind::caught_up);
    CHECK(second_checkpoint.kind == history_detail::cursor_step_kind::caught_up);
    CHECK(first_checkpoint.checkpoint_qpc == 200U);
    CHECK(second_checkpoint.checkpoint_qpc == 200U);
}

TEST_CASE("gameinput.history.source_cursor_requires_native_horizon_to_cover_guide_floor",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.capture->set_callback_coverage(true);
    fixture.capture->on_seed(reading_parts{
        .guide = true,
        .source_timestamp_us = 200U,
        .observation_qpc = 200U,
    });
    fixture.native.history.steps[0U] = caught_up_step(150U);
    fixture.native.history.steps[1U] = caught_up_step(200U);
    fixture.native.history.step_count = 2U;

    const auto seed = fixture.cursor.start(history_time{});
    REQUIRE(seed.has_value());
    REQUIRE(seed->baseline.effective_qpc == 200U);

    const auto behind = fixture.cursor.next(history_time{});
    CHECK(behind.kind == history_detail::cursor_step_kind::progress);
    CHECK(behind.checkpoint_qpc == 0U);

    const auto at_floor = fixture.cursor.next(history_time{});
    CHECK(at_floor.kind == history_detail::cursor_step_kind::caught_up);
    CHECK(at_floor.checkpoint_qpc == 200U);
}

TEST_CASE("gameinput.history.source_cursor_never_qualifies_checkpoint_without_coverage_and_guide",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    SECTION("coverage down") {
        cursor_fixture fixture;
        fixture.native.history.steps[0U] =
            reading_step(static_cast<button_mask>(gamepad_button::south), 110U, 110U);
        fixture.native.history.steps[1U] = caught_up_step(120U);
        fixture.native.history.step_count = 2U;
        REQUIRE(fixture.cursor.start(history_time{}).has_value());

        const auto reading = fixture.cursor.next(history_time{});
        REQUIRE(reading.kind == history_detail::cursor_step_kind::reading);
        CHECK(reading.reading.state.buttons == static_cast<button_mask>(gamepad_button::south));

        const auto checkpoint = fixture.cursor.next(history_time{});
        CHECK(checkpoint.kind == history_detail::cursor_step_kind::progress);
        CHECK(checkpoint.checkpoint_qpc == 0U);
    }

    SECTION("coverage up without a fresh guide") {
        cursor_fixture fixture;
        fixture.capture->set_callback_coverage(true);
        REQUIRE(fixture.cursor.start(history_time{}).has_value());

        const auto step = fixture.cursor.next(history_time{});
        CHECK(step.kind == history_detail::cursor_step_kind::progress);
        CHECK(step.checkpoint_qpc == 0U);
    }

    SECTION("coverage activation discards an older guide fact from the native baseline") {
        cursor_fixture fixture;
        fixture.capture->on_seed(reading_parts{
            .guide = true,
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
        });
        fixture.capture->set_callback_coverage(true);

        const auto seed = fixture.cursor.start(history_time{});
        REQUIRE(seed.has_value());
        CHECK_FALSE(seed->baseline.guide_known);
        CHECK((seed->baseline.state.buttons & guide_mask) == 0U);
    }
}

TEST_CASE("gameinput.history.source_cursor_native_pad_wins_over_callback_pad",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.native.history.steps[0U] =
        reading_step(static_cast<button_mask>(gamepad_button::south), 120U, 120U);
    fixture.native.history.step_count = 1U;
    REQUIRE(fixture.cursor.start(history_time{}).has_value());

    fixture.capture->on_callback(reading_parts{
        .pad = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)},
        .source_timestamp_us = 110U,
        .observation_qpc = 110U,
    });

    CHECK(fixture.cursor.next(history_time{}).kind == history_detail::cursor_step_kind::progress);
    const auto native = fixture.cursor.next(history_time{});
    REQUIRE(native.kind == history_detail::cursor_step_kind::reading);
    CHECK(native.reading.state.buttons == static_cast<button_mask>(gamepad_button::south));
}

TEST_CASE("gameinput.history.source_cursor_clamps_late_guide_without_rewriting_source_time",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.native.history.seed.baseline.observation_qpc = 100U;
    fixture.native.history.quiet_checkpoint_qpc = 150U;

    REQUIRE(fixture.cursor.start(history_time{}).has_value());
    fixture.capture->on_callback(reading_parts{
        .guide = true,
        .source_timestamp_us = 40U,
        .observation_qpc = 90U,
    });

    const auto guide = fixture.cursor.next(history_time{});
    REQUIRE(guide.kind == history_detail::cursor_step_kind::reading);
    CHECK(guide.reading.source_timestamp_us == 40U);
    CHECK(guide.reading.effective_qpc == 100U);
    CHECK(guide.reading.guide_known);
}

TEST_CASE("gameinput.history.source_cursor_retries_native_prefix_on_same_cursor",
    "[component][gameinput][history][source]") {
    scripted_now = history_time{};
    cursor_fixture fixture;
    fixture.qualify_guide();
    fixture.native.history.steps[0U] =
        reading_step(static_cast<button_mask>(gamepad_button::south), 110U, 110U);
    fixture.native.history.steps[1U] = native_history_step{
        .kind = native_history_step_kind::provider_failure,
        .native_code = 17U,
    };
    fixture.native.history.steps[2U] = caught_up_step(120U);
    fixture.native.history.step_count = 3U;
    REQUIRE(fixture.cursor.start(history_time{}).has_value());

    const auto prefix = fixture.cursor.next(history_time{});
    REQUIRE(prefix.kind == history_detail::cursor_step_kind::reading);
    const auto failure = fixture.cursor.next(history_time{});
    CHECK(failure.kind == history_detail::cursor_step_kind::provider_failure);
    CHECK(failure.native_code == 17U);
    const auto retried = fixture.cursor.next(history_time{});
    CHECK(retried.kind == history_detail::cursor_step_kind::caught_up);
    CHECK(fixture.native.create_count == 1U);
    CHECK(fixture.native.history.next_count == 3U);
}

} // namespace
} // namespace inputnext::gameinput_detail
