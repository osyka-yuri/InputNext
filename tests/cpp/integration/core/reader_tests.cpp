#include "core_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_history.hpp"
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("core.reader_retains_a_full_batch_after_a_short_buffer_probe",
    "[integration][core][reader]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());

    const auto baseline = fixture.core->read_events(*opened, 1U);
    REQUIRE(baseline.outcome == inputnext::core::status::ok);
    REQUIRE(baseline.batch.count == 1U);
    CHECK(baseline.batch.reset == inputnext::history_reset::initial_open);

    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::caught_up,
        .checkpoint_qpc = 1'200U});
    const auto checkpoint = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    CHECK(checkpoint.outcome == inputnext::core::status::no_data);
    CHECK(checkpoint.batch.count == 0U);
    CHECK(checkpoint.batch.caught_up);
    CHECK(checkpoint.batch.checkpoint_qpc != 0U);

    const auto script = fixture.scripts[0U];
    script->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(7U),
            .source_timestamp_us = 130U,
            .observation_qpc = 1'300U}});
    script->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(9U),
            .source_timestamp_us = 140U,
            .observation_qpc = 1'400U}});

    const auto probe = fixture.core->read_events(*opened, 1U);
    REQUIRE(probe.outcome == inputnext::core::status::buffer_too_small);
    REQUIRE(probe.batch.count == 2U);
    CHECK(probe.batch.first_sequence != 0U);

    const auto replay = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    CHECK(replay.outcome == inputnext::core::status::ok);
    CHECK(replay.batch == probe.batch);
    CHECK(replay.batch.records[0U].sample.state.buttons == 7U);
    CHECK(replay.batch.records[1U].sample.state.buttons == 9U);
    CHECK(fixture.core->read_events(*opened, inputnext::history_batch_capacity).outcome ==
          inputnext::core::status::no_data);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::stale_reader);
}

TEST_CASE("core.reader_preserves_a_valid_prefix_when_history_traversal_fails",
    "[integration][core][reader]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());
    REQUIRE(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(13U),
            .source_timestamp_us = 160U,
            .observation_qpc = 1'600U}});
    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::provider_failure,
        .native_code = 0x1234U});

    const auto result = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    CHECK(result.outcome == inputnext::core::status::io_failed);
    REQUIRE(result.batch.count == 1U);
    CHECK(result.batch.records[0U].sample.state.buttons == 13U);
    const auto& failure = inputnext::core_test_support::required_value(result.batch.failure);
    CHECK(failure.native_code == 0x1234U);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
}

TEST_CASE("core.reader_does_not_replay_a_pending_batch_after_source_retirement",
    "[integration][core][reader][retirement]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());
    REQUIRE(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(17U),
            .source_timestamp_us = 170U,
            .observation_qpc = 1'700U}});
    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(19U),
            .source_timestamp_us = 180U,
            .observation_qpc = 1'800U}});

    const auto short_read = fixture.core->read_events(*opened, 1U);
    REQUIRE(short_read.outcome == inputnext::core::status::buffer_too_small);
    REQUIRE(short_read.batch.count == 2U);
    REQUIRE(fixture.gameinput.registry.retire(target).has_value());

    const auto retired = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    CHECK(retired.outcome == inputnext::core::status::source_retired);
    CHECK(retired.batch.count == 0U);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
}
