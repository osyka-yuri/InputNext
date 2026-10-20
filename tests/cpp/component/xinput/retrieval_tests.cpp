#include "support/xinput/controlled_event_source.hpp"

#include "support/xinput/event_fixture.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <utility>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

constexpr std::uint64_t qpc_frequency = 1000U;

[[nodiscard]] xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto result = service.assign(connection.connection.target, connection.connection.reader);
    if (!result.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *result;
}

void prepare_key(xinput::service& service,
    controlled_event_source& events,
    DWORD user_index,
    connection_target target,
    std::uint64_t stream,
    button_mask buttons,
    history_time now) {
    events.enqueue(user_index, initial_batch(target, stream, {}, 100U));
    service.pump_events(user_index, now);
    events.enqueue(user_index,
        history_events(target,
            event_stream_id{stream},
            2U,
            {{gamepad_state{.buttons = buttons}, 200U}},
            {},
            200U));
    service.pump_events(user_index, now);
}

using retrieval_result = std::pair<DWORD, XINPUT_KEYSTROKE>;

[[nodiscard]] std::future<retrieval_result> retrieve_async(xinput::service& service,
    DWORD user_index) {
    return std::async(std::launch::async, [&service, user_index] {
        XINPUT_KEYSTROKE output{};
        const auto status = service.get_keystroke(xinput::flavor::v1_4, user_index, 0U, &output);
        return retrieval_result{status, output};
    });
}

[[nodiscard]] bool ready_within(std::future<retrieval_result> const& future) {
    return future.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
}

} // namespace

TEST_CASE("specific and ANY retrieval leave prepared and pending history to explicit maintenance",
    "[component][xinput][events][retrieval]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    prepare_key(service,
        events,
        first_assignment.user_index,
        first.connection.target,
        201U,
        static_cast<button_mask>(gamepad_button::south),
        focus_state.now());
    prepare_key(service,
        events,
        second_assignment.user_index,
        second.connection.target,
        202U,
        static_cast<button_mask>(gamepad_button::east),
        focus_state.now());

    const auto opens_before = events.open_calls();
    const auto reads_before = events.read_calls();
    const auto closes_before = events.close_calls();
    XINPUT_KEYSTROKE output{};
    output.HidCode = 0xA5U;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, first_assignment.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, 0x000000FFU, 0U, &output) == ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5801U);

    output.HidCode = 0x5AU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, first_assignment.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0x5AU);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, 0x000000FFU, 0U, &output) == ERROR_EMPTY);
    CHECK(output.HidCode == 0x5AU);
    CHECK(events.open_calls() == opens_before);
    CHECK(events.read_calls() == reads_before);
    CHECK(events.close_calls() == closes_before);
}

TEST_CASE("prepared retrieval rejects stale qualification without provider work",
    "[component][xinput][events][retrieval][lifecycle]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto replacement = devices.add();
    const auto occupant = assign(service, connection);
    prepare_key(service,
        events,
        occupant.user_index,
        connection.connection.target,
        205U,
        static_cast<button_mask>(gamepad_button::south),
        focus_state.now());

    const auto verify_retrieval = [&](DWORD expected_status) {
        const auto opens_before = events.open_calls();
        const auto reads_before = events.read_calls();
        const auto closes_before = events.close_calls();
        XINPUT_KEYSTROKE output{};
        output.HidCode = 0xC3U;
        CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
              expected_status);
        CHECK(output.HidCode == 0xC3U);
        output.HidCode = 0xC4U;
        CHECK(service.get_keystroke(xinput::flavor::v1_4, 0x000000FFU, 0U, &output) ==
              expected_status);
        CHECK(output.HidCode == 0xC4U);
        CHECK(events.open_calls() == opens_before);
        CHECK(events.read_calls() == reads_before);
        CHECK(events.close_calls() == closes_before);
    };

    SECTION("retirement rejects the prepared specific and ANY result") {
        REQUIRE(devices.registry().retire(connection.connection.target).has_value());
        CHECK(service.positions()[occupant.user_index] == occupant);
        verify_retrieval(ERROR_DEVICE_NOT_CONNECTED);
    }

    SECTION("reassignment does not expose a previous occupant's prepared key") {
        REQUIRE(service.unassign(occupant).has_value());
        static_cast<void>(assign(service, replacement));
        verify_retrieval(ERROR_EMPTY);
    }

    SECTION("focus loss rejects a prepared key") {
        focus_state.set(focus::evidence::background);
        verify_retrieval(ERROR_EMPTY);
    }

    SECTION("expired foreground evidence rejects a prepared key") {
        focus_state.block_next_observation();
        REQUIRE(focus_state.wait_for_blocked_observation(std::chrono::seconds{2}));
        focus_state.advance(std::chrono::milliseconds{251});
        verify_retrieval(ERROR_EMPTY);
        focus_state.release_blocked_observation();
    }

    SECTION("disable then enable rejects a prepared key") {
        service.enable(FALSE);
        service.enable(TRUE);
        verify_retrieval(ERROR_EMPTY);
    }

    SECTION("close rejects a prepared key") {
        service.close();
        verify_retrieval(ERROR_DEVICE_NOT_CONNECTED);
    }
}

TEST_CASE("prepared retrieval completes while explicit maintenance is blocked opening",
    "[component][xinput][events][retrieval][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    prepare_key(service,
        events,
        second_assignment.user_index,
        second.connection.target,
        211U,
        static_cast<button_mask>(gamepad_button::east),
        focus_state.now());

    events.block_next_open();
    std::future<void> blocked_pump;
    std::future<retrieval_result> specific;
    std::future<retrieval_result> any;
    const release_event_source_on_exit release{events};
    blocked_pump = std::async(std::launch::async,
        [&] { service.pump_events(first_assignment.user_index, focus_state.now()); });
    REQUIRE(events.wait_for_open(std::chrono::seconds{2}));

    const auto opens_while_blocked = events.open_calls();
    const auto reads_while_blocked = events.read_calls();
    const auto closes_while_blocked = events.close_calls();
    specific = retrieve_async(service, first_assignment.user_index);
    any = retrieve_async(service, 0x000000FFU);
    const bool specific_ready = ready_within(specific);
    CHECK(specific_ready);
    if (specific_ready) {
        CHECK(specific.get().first == ERROR_EMPTY);
    }
    const bool any_ready = ready_within(any);
    CHECK(any_ready);
    if (any_ready) {
        const auto [status, output] = any.get();
        CHECK(status == ERROR_SUCCESS);
        CHECK(output.VirtualKey == 0x5801U);
    }
    CHECK(events.open_calls() == opens_while_blocked);
    CHECK(events.read_calls() == reads_while_blocked);
    CHECK(events.close_calls() == closes_while_blocked);

    events.release_open();
    CHECK(blocked_pump.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    if (blocked_pump.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
        blocked_pump.get();
    }
}

TEST_CASE("specific and ANY retrieval complete during a blocked read with prepared keys",
    "[component][xinput][events][retrieval][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    prepare_key(service,
        events,
        first_assignment.user_index,
        first.connection.target,
        221U,
        static_cast<button_mask>(gamepad_button::south),
        focus_state.now());
    prepare_key(service,
        events,
        second_assignment.user_index,
        second.connection.target,
        222U,
        static_cast<button_mask>(gamepad_button::east),
        focus_state.now());
    events.enqueue(first_assignment.user_index,
        history_events(first.connection.target,
            event_stream_id{221U},
            3U,
            {{gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}, 300U}},
            {},
            300U));

    events.block_next_read();
    std::future<void> blocked_pump;
    std::future<retrieval_result> specific;
    std::future<retrieval_result> any;
    std::future<retrieval_result> empty;
    const release_event_source_on_exit release{events};
    blocked_pump = std::async(std::launch::async,
        [&] { service.pump_events(first_assignment.user_index, focus_state.now()); });
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));

    const auto opens_while_blocked = events.open_calls();
    const auto reads_while_blocked = events.read_calls();
    const auto closes_while_blocked = events.close_calls();
    specific = retrieve_async(service, first_assignment.user_index);
    const bool specific_ready = ready_within(specific);
    REQUIRE(specific_ready);
    const auto [specific_status, specific_output] = specific.get();
    CHECK(specific_status == ERROR_SUCCESS);
    CHECK(specific_output.VirtualKey == 0x5800U);

    any = retrieve_async(service, 0x000000FFU);
    const bool any_ready = ready_within(any);
    REQUIRE(any_ready);
    const auto [any_status, any_output] = any.get();
    CHECK(any_status == ERROR_SUCCESS);
    CHECK(any_output.VirtualKey == 0x5801U);

    empty = retrieve_async(service, first_assignment.user_index);
    const bool empty_ready = ready_within(empty);
    REQUIRE(empty_ready);
    CHECK(empty.get().first == ERROR_EMPTY);
    CHECK(events.open_calls() == opens_while_blocked);
    CHECK(events.read_calls() == reads_while_blocked);
    CHECK(events.close_calls() == closes_while_blocked);

    events.release_read();
    CHECK(blocked_pump.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    if (blocked_pump.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
        blocked_pump.get();
    }
}

TEST_CASE("specific and ANY retrieval complete during a blocked close",
    "[component][xinput][events][retrieval][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    prepare_key(service,
        events,
        first_assignment.user_index,
        first.connection.target,
        231U,
        static_cast<button_mask>(gamepad_button::south),
        focus_state.now());
    prepare_key(service,
        events,
        second_assignment.user_index,
        second.connection.target,
        232U,
        static_cast<button_mask>(gamepad_button::east),
        focus_state.now());

    events.block_next_close();
    std::future<bool> blocked_unassign;
    std::future<retrieval_result> specific;
    std::future<retrieval_result> any;
    const release_event_source_on_exit release{events};
    blocked_unassign = std::async(std::launch::async,
        [&] { return service.unassign(first_assignment).has_value(); });
    REQUIRE(events.wait_for_close(std::chrono::seconds{2}));

    const auto opens_while_blocked = events.open_calls();
    const auto reads_while_blocked = events.read_calls();
    const auto closes_while_blocked = events.close_calls();
    specific = retrieve_async(service, first_assignment.user_index);
    any = retrieve_async(service, 0x000000FFU);
    const bool specific_ready = ready_within(specific);
    CHECK(specific_ready);
    if (specific_ready) {
        CHECK(specific.get().first == ERROR_DEVICE_NOT_CONNECTED);
    }
    const bool any_ready = ready_within(any);
    CHECK(any_ready);
    if (any_ready) {
        const auto [status, output] = any.get();
        CHECK(status == ERROR_SUCCESS);
        CHECK(output.VirtualKey == 0x5801U);
    }
    CHECK(events.open_calls() == opens_while_blocked);
    CHECK(events.read_calls() == reads_while_blocked);
    CHECK(events.close_calls() == closes_while_blocked);

    events.release_close();
    CHECK(blocked_unassign.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    if (blocked_unassign.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
        CHECK(blocked_unassign.get());
    }
}
