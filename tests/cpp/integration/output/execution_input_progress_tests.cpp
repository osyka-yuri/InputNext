#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/controlled_native_sink.hpp"
#include "support/xinput/controlled_event_source.hpp"
#include "support/xinput/event_fixture.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
namespace xinput_support = inputnext::xinput_test_support;

class release_blocked_write final {
public:
    explicit release_blocked_write(std::shared_ptr<controlled_native_state> state) noexcept
        : state_(std::move(state)) {}

    ~release_blocked_write() noexcept { state_->release_write(); }

    release_blocked_write(const release_blocked_write&) = delete;
    release_blocked_write& operator=(const release_blocked_write&) = delete;
    release_blocked_write(release_blocked_write&&) = delete;
    release_blocked_write& operator=(release_blocked_write&&) = delete;

private:
    std::shared_ptr<controlled_native_state> state_;
};

void prepare_key(xinput::service& service,
    xinput_support::controlled_event_source& events,
    const DWORD user_index,
    const connection_target target,
    const std::uint64_t stream,
    const history_time now) {
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

TEST_CASE("output.blocked_native_delivery_does_not_block_input_focus_or_xinput_progress",
    "[integration][output][executor]") {
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
    auto created = executor::create(fixture.owner);
    REQUIRE(created.has_value());
    const auto running = std::move(*created);
    const auto state = std::make_shared<controlled_native_state>();
    state->block_next_write();
    const release_blocked_write release_write{state};
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(state);
    const auto backend =
        running->register_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    const bool native_write_blocked = state->wait_for_blocked_write();
    REQUIRE(native_write_blocked);
    const auto focus_observations_before =
        fixture.focus_observer.published().completed_observations;
    const auto source_entries_before = fixture.focus_source().observations();

    const input_update update{
        .state = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east),
            .left_stick = stick_state{.x = 0.4F, .y = -0.3F},
            .right_stick = {},
            .left_trigger = 0.6F,
            .right_trigger = 0.1F},
        .source_timestamp_us = 123456U,
        .observation_qpc = 789012U,
    };
    const auto published = fixture.connection.producer.publish(update);
    REQUIRE(published.has_value());
    const auto latest = fixture.connection.reader.read();
    REQUIRE(latest.has_value());

    XINPUT_STATE xinput_state{};
    const auto state_status =
        xinput_service.get_state(xinput::flavor::v1_4, assignment->user_index, &xinput_state);
    XINPUT_KEYSTROKE key{};
    const auto key_status =
        xinput_service.get_keystroke(xinput::flavor::v1_4, assignment->user_index, 0U, &key);
    // Entering a following observation requires the preceding observation to have been published.
    const bool following_observation_entered =
        fixture.focus_source().wait_for_observations(source_entries_before + 2U);
    const auto focus_after = fixture.focus_observer.published();
    const auto opens_after = events.open_calls();
    const auto reads_after = events.read_calls();
    const auto closes_after = events.close_calls();

    state->release_write();
    running->shutdown();

    CHECK(published.value() == 2U);
    CHECK(latest->sample.revision == 2U);
    CHECK(latest->sample.state == update.state);
    CHECK(latest->sample.observation_qpc == update.observation_qpc);
    CHECK(state_status == ERROR_SUCCESS);
    CHECK(xinput_state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
    CHECK(key_status == ERROR_SUCCESS);
    CHECK(key.VirtualKey == 0x5800U);
    CHECK(opens_after == opens_before);
    CHECK(reads_after == reads_before);
    CHECK(closes_after == closes_before);
    CHECK(following_observation_entered);
    CHECK(focus_after.completed_observations > focus_observations_before);
    CHECK(native_write_blocked);
    CHECK(state->destruction_count() == 1U);
}

} // namespace
