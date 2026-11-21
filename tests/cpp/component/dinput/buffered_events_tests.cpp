#include "dinput/buffered/subscription.hpp"
#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "history/reader_observation.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace {

using namespace inputnext;
using namespace inputnext::dinput;
using namespace inputnext::history_detail;
constexpr auto pressed = static_cast<button_mask>(gamepad_button::south);

[[nodiscard]] connection_target target() noexcept {
    connection_target result{.device = device_key{1U}, .connection = connection_id{1U}};
    result.runtime.bytes.front() = std::byte{1U};
    return result;
}

[[nodiscard]] history_sample sample(button_mask buttons, std::uint64_t timestamp) noexcept {
    return history_sample{.state = gamepad_state{.buttons = buttons},
        .source_timestamp_us = timestamp,
        .effective_qpc = timestamp * 10U,
        .guide_known = true};
}

[[nodiscard]] reader_boundary cut() noexcept {
    return reader_boundary{.domain = history_domain{.target = target(), .stream = 10U},
        .sequence = 1U,
        .checkpoint_qpc = 10'000U,
        .clock = source_clock_sample{.timestamp_us = 1'000U, .system_time_ms = 20'000U}};
}

[[nodiscard]] buffer_configuration configuration(DWORD capacity = 8U) {
    DIOBJECTDATAFORMAT object{.pguid = &GUID_Button,
        .dwOfs = 0U,
        .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0U),
        .dwFlags = 0U};
    const DIDATAFORMAT source{.dwSize = sizeof(DIDATAFORMAT),
        .dwObjSize = sizeof(DIOBJECTDATAFORMAT),
        .dwFlags = DIDF_ABSAXIS,
        .dwDataSize = sizeof(DWORD),
        .dwNumObjs = 1U,
        .rgodf = &object};
    buffer_configuration result{.target = target(), .requested_size = capacity};
    REQUIRE(compile_format(source, result.format, result.model) == DI_OK);
    return result;
}

[[nodiscard]] observed_read transition(button_mask buttons,
    std::uint64_t timestamp,
    std::uint64_t sequence,
    history_record_origin origin = history_record_origin::native_pad) noexcept {
    observed_read result{};
    auto& batch = result.batch;
    batch.domain = cut().domain;
    batch.source = history_source_kind::native_history;
    batch.count = 1U;
    batch.first_sequence = sequence;
    batch.last_sequence = sequence;
    batch.records.front() = history_record{.sample = sample(buttons, timestamp),
        .sequence = sequence,
        .origin = origin};
    return result;
}

[[nodiscard]] observed_read checkpoint(std::uint64_t sequence) noexcept {
    observed_read result{.fresh_checkpoint = true};
    result.batch.domain = cut().domain;
    result.batch.source = history_source_kind::native_history;
    result.batch.caught_up = true;
    result.batch.checkpoint_sequence = sequence;
    result.batch.checkpoint_qpc = 20'000U;
    return result;
}

void accept(buffered_subscription& subscriber, const observed_read& read, DWORD sequence = 41U) {
    std::array<DWORD, history_batch_capacity> sequences{};
    sequences.front() = sequence;
    subscriber.accept(read, sequences);
}

[[nodiscard]] HRESULT read_events(buffered_subscription& subscriber,
    std::array<DIDEVICEOBJECTDATA, 8U>& events,
    DWORD& returned,
    bool peek = false) noexcept {
    return subscriber.read(sizeof(DIDEVICEOBJECTDATA),
        events.data(),
        static_cast<DWORD>(events.size()),
        returned,
        peek);
}

} // namespace

TEST_CASE("dinput.buffered_prepared_queue_keeps_events_until_activation",
    "[component][dinput][buffered][acquisition]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.wait_ready() == DI_OK);
    accept(subscriber, transition(pressed, 1'001U, 2U));

    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    CHECK(read_events(subscriber, events, count) == DIERR_NOTACQUIRED);
    REQUIRE(subscriber.activate());
    REQUIRE(read_events(subscriber, events, count) == DI_OK);
    REQUIRE(count == 1U);
    CHECK(events.front().dwOfs == 0U);
    CHECK(events.front().dwData == 0x80U);
    CHECK(events.front().dwSequence == 41U);
    CHECK(events.front().uAppData == 0U);
}

TEST_CASE("dinput.buffered_source_time_defines_cut_while_equal_later_ticks_keep_order",
    "[component][dinput][buffered][boundary]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.activate());

    // Historical Pad input updates the predecessor without becoming a new event.
    accept(subscriber, transition(pressed, 900U, 2U));
    accept(subscriber, transition(0U, 1'001U, 3U), 50U);
    accept(subscriber, transition(pressed, 1'001U, 4U), 51U);
    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    REQUIRE(read_events(subscriber, events, count) == DI_OK);
    REQUIRE(count == 2U);
    CHECK(events[0U].dwData == 0U);
    CHECK(events[1U].dwData == 0x80U);
    CHECK(events[0U].dwTimeStamp == events[1U].dwTimeStamp);
    CHECK(events[0U].dwSequence == 50U);
    CHECK(events[1U].dwSequence == 51U);
}

TEST_CASE("dinput.buffered_equal_cut_tick_reports_loss_and_resynchronizes",
    "[component][dinput][buffered][boundary][loss]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.activate());
    accept(subscriber, transition(pressed, 1'000U, 2U));

    DWORD count{};
    REQUIRE(subscriber.read(sizeof(DIDEVICEOBJECTDATA), nullptr, 0U, count, false) ==
            DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
    accept(subscriber, checkpoint(2U));
    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    REQUIRE(read_events(subscriber, events, count, true) == DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
    REQUIRE(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
    CHECK(read_events(subscriber, events, count) == DI_OK);

    accept(subscriber, transition(0U, 1'001U, 3U));
    REQUIRE(read_events(subscriber, events, count) == DI_OK);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0U);
}

TEST_CASE("dinput.buffered_late_guide_keeps_valid_prefix_and_resets_predecessor",
    "[component][dinput][buffered][boundary][guide][loss]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.activate());
    accept(subscriber, transition(pressed, 1'001U, 2U));
    accept(subscriber, transition(pressed, 900U, 3U, history_record_origin::callback_guide));
    accept(subscriber, checkpoint(3U));

    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    REQUIRE(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0x80U);
    accept(subscriber, transition(0U, 1'002U, 4U));
    REQUIRE(read_events(subscriber, events, count) == DI_OK);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0U);
}

TEST_CASE("dinput.buffered_busy_does_not_invalidate_continuity_and_failure_preserves_prefix",
    "[component][dinput][buffered][loss]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.activate());
    observed_read busy{};
    busy.batch.domain = cut().domain;
    busy.batch.failure = history_failure{.code = history_error::busy};
    accept(subscriber, busy);

    auto prefix = transition(pressed, 1'001U, 2U);
    prefix.batch.failure = history_failure{.code = history_error::provider_failure};
    accept(subscriber, prefix);
    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    REQUIRE(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0x80U);
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    accept(subscriber, checkpoint(2U));
    REQUIRE(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    CHECK(read_events(subscriber, events, count) == DI_OK);
}

TEST_CASE("dinput.buffered_unqualified_cut_acquires_with_loss_until_a_fresh_checkpoint",
    "[component][dinput][buffered][boundary][recovery]") {
    buffered_subscription subscriber{configuration()};
    auto boundary = cut();
    boundary.checkpoint_qpc = 0U;
    boundary.sequence = 0U;
    subscriber.prepare(boundary, std::nullopt);
    REQUIRE(subscriber.wait_ready() == DI_OK);
    REQUIRE(subscriber.activate());

    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    accept(subscriber, transition(pressed, 1'001U, 1U));
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
    auto inherited = checkpoint(1U);
    inherited.fresh_checkpoint = false;
    accept(subscriber, inherited);
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    accept(subscriber, checkpoint(1U));
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    CHECK(read_events(subscriber, events, count) == DI_OK);

    accept(subscriber, transition(0U, 1'002U, 2U));
    REQUIRE(read_events(subscriber, events, count) == DI_OK);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0U);
}

TEST_CASE("dinput.buffered_first_post_cut_sequence_must_follow_the_boundary",
    "[component][dinput][buffered][boundary][loss]") {
    buffered_subscription subscriber{configuration()};
    subscriber.prepare(cut(), sample(0U, 100U));
    REQUIRE(subscriber.activate());
    accept(subscriber, transition(pressed, 1'001U, 4U));
    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    CHECK(read_events(subscriber, events, count) == DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
}
