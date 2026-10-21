#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "support/xinput/controlled_event_source.hpp"
#include "support/xinput/event_fixture.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;

namespace xinput_support = inputnext::xinput_test_support;

void prepare_key(xinput::service& service,
    xinput_support::controlled_event_source& events,
    DWORD user_index,
    connection_target target,
    std::uint64_t stream,
    history_time now) {
    events.enqueue(user_index, xinput_support::initial_batch(target, stream, {}, 100U));
    service.pump_events(user_index, now);
    events.enqueue(user_index,
        xinput_support::history_events(target,
            xinput_support::event_stream_id{stream},
            2U,
            {{gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)}, 200U}},
            {},
            200U));
    service.pump_events(user_index, now);
}

TEST_CASE("output.input_publication_and_latest_read_progress_while_gate_is_held",
    "[integration][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());

    xinput_support::metadata_source metadata;
    xinput_support::controlled_event_source events;
    xinput::service xinput_service{fixture.focus_observer, metadata, events, 1000U};
    const auto assignment =
        xinput_service.assign(fixture.connection.target, fixture.connection.reader);
    REQUIRE(assignment.has_value());

    const input_update initial_update{
        .state = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)},
        .source_timestamp_us = 100U,
        .observation_qpc = 100U,
    };
    REQUIRE(fixture.connection.producer.publish(initial_update).has_value());
    XINPUT_STATE initial_state{};
    REQUIRE(
        xinput_service.get_state(xinput::flavor::v1_4, assignment->user_index, &initial_state) ==
        ERROR_SUCCESS);
    prepare_key(xinput_service,
        events,
        assignment->user_index,
        fixture.connection.target,
        771U,
        std::chrono::steady_clock::now());
    const auto opens_before = events.open_calls();
    const auto reads_before = events.read_calls();
    const auto closes_before = events.close_calls();

    auto gate = inputnext::output::detail::authority_test_access::lock_gate(fixture.owner);
    const input_update update{
        .state =
            gamepad_state{
                .buttons = static_cast<button_mask>(gamepad_button::east),
                .left_stick = stick_state{.x = 0.4F, .y = -0.3F},
                .right_stick = {},
                .left_trigger = 0.6F,
                .right_trigger = 0.1F,
            },
        .source_timestamp_us = 123456U,
        .observation_qpc = 789012U,
    };
    const auto published = fixture.connection.producer.publish(update);
    REQUIRE(published.has_value());
    CHECK(published.value() == 2U);

    const auto latest = fixture.connection.reader.read();
    REQUIRE(latest.has_value());
    CHECK(latest->sample.revision == 2U);
    CHECK(latest->sample.state == update.state);
    CHECK(latest->sample.observation_qpc == update.observation_qpc);

    XINPUT_STATE xinput_state{};
    REQUIRE(xinput_service.get_state(xinput::flavor::v1_4, assignment->user_index, &xinput_state) ==
            ERROR_SUCCESS);
    CHECK(xinput_state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
    XINPUT_KEYSTROKE key{};
    REQUIRE(xinput_service.get_keystroke(xinput::flavor::v1_4, assignment->user_index, 0U, &key) ==
            ERROR_SUCCESS);
    CHECK(key.VirtualKey == 0x5800U);
    CHECK(events.open_calls() == opens_before);
    CHECK(events.read_calls() == reads_before);
    CHECK(events.close_calls() == closes_before);
    gate.unlock();

    const auto output_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(output_snapshot.has_value());
    CHECK(output_snapshot->present_channels() == 0U);
}

} // namespace
