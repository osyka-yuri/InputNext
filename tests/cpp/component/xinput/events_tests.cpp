#include "support/xinput/controlled_event_source.hpp"

#include "support/xinput/event_fixture.hpp"
#include "support/xinput/service_test_access.hpp"
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
#include <thread>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

constexpr DWORD any_user = 0x000000FFU;
constexpr std::uint64_t qpc_frequency = 1000U;

[[nodiscard]] xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto result = service.assign(connection.connection.target, connection.connection.reader);
    if (!result.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *result;
}

[[nodiscard]] bool wait_until(const auto& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

[[nodiscard]] gamepad_state with_buttons(button_mask buttons) noexcept {
    return gamepad_state{.buttons = buttons};
}

} // namespace

TEST_CASE("GetKeystroke preserves standard argument, rotation, and output behavior",
    "[component][xinput][events]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);

    XINPUT_KEYSTROKE output{};
    output.HidCode = 0xFFU;
    CHECK(service.get_keystroke(xinput::flavor::v9_1_0, 17U, 0U, nullptr) == ERROR_NOT_SUPPORTED);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, 4U, 0U, nullptr) == ERROR_BAD_ARGUMENTS);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, first_assignment.user_index, 7U, nullptr) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(
        service.get_keystroke(xinput::flavor::v1_4, 2U, 0U, &output) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(output.HidCode == 0xFFU);

    events.enqueue(first_assignment.user_index,
        initial_batch(first.connection.target, 41U, {}, 100U));
    events.enqueue(second_assignment.user_index,
        initial_batch(second.connection.target,
            42U,
            with_buttons(static_cast<button_mask>(gamepad_button::guide)),
            100U));
    service.pump_events(first_assignment.user_index, focus_state.now());
    service.pump_events(second_assignment.user_index, focus_state.now());

    const auto first_key = with_buttons(static_cast<button_mask>(gamepad_button::south));
    const auto second_key = with_buttons(static_cast<button_mask>(gamepad_button::east) |
                                         static_cast<button_mask>(gamepad_button::guide));
    events.enqueue(first_assignment.user_index,
        history_events(first.connection.target,
            event_stream_id{41U},
            2U,
            {{first_key, 200U}},
            {},
            200U));
    events.enqueue(second_assignment.user_index,
        history_events(second.connection.target,
            event_stream_id{42U},
            2U,
            {{second_key, 200U}},
            {},
            200U));
    service.pump_events(first_assignment.user_index, focus_state.now());
    service.pump_events(second_assignment.user_index, focus_state.now());

    output = {};
    CHECK(service.get_keystroke(xinput::flavor::v1_3, any_user, 0xDEADU, &output) == ERROR_SUCCESS);
    CHECK(output.UserIndex == first_assignment.user_index);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
    CHECK(output.HidCode == 0U);

    output = {};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, any_user, 0U, &output) == ERROR_SUCCESS);
    CHECK(output.UserIndex == second_assignment.user_index);
    CHECK(output.VirtualKey == 0x5801U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);

    output.HidCode = 0xCDU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, any_user, 0U, &output) == ERROR_EMPTY);
    CHECK(output.HidCode == 0xCDU);

    events.enqueue(first_assignment.user_index,
        history_events(first.connection.target,
            event_stream_id{41U},
            3U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::east)), 300U}},
            {},
            300U));
    service.pump_events(first_assignment.user_index, focus_state.now());
    output = {};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, any_user, 0U, &output) == ERROR_SUCCESS);
    CHECK(output.UserIndex == first_assignment.user_index);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYUP);
}

TEST_CASE("qualified batches cut positive failure prefixes and preserve zero-prefix busy progress",
    "[component][xinput][events][recovery]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 51U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());

    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{51U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            history_failure{.code = history_error::provider_failure}));
    service.pump_events(occupant.user_index, focus_state.now());
    XINPUT_KEYSTROKE output{};
    output.HidCode = 0x5AU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0x5AU);
    CHECK_FALSE(events.is_open(occupant.user_index));

    const auto recovery = initial_batch(connection.connection.target, 52U, {}, 300U);
    events.enqueue(occupant.user_index, recovery);
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{52U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::east)), 400U}},
            {},
            400U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{52U},
            3U,
            {},
            history_failure{.code = history_error::busy}));
    service.pump_events(occupant.user_index, focus_state.now());
    output = {};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5801U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
}

TEST_CASE("tracker overflow cannot expose a partial event prefix",
    "[component][xinput][events][recovery]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 61U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());

    const auto full =
        gamepad_state{.buttons = 0x3FFFU, .left_trigger = 1.0F, .right_trigger = 1.0F};
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{61U},
            2U,
            {{full, 100U}, {{}, 200U}, {full, 300U}, {{}, 400U}, {full, 500U}}));
    service.pump_events(occupant.user_index, focus_state.now());

    XINPUT_KEYSTROKE output{};
    output.HidCode = 0x1EU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0x1EU);
}

TEST_CASE("repeat authority follows consumed QPC checkpoints and events win their deadline",
    "[component][xinput][events][repeat]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto held = devices.add();
    const auto deadline = devices.add();
    const auto held_assignment = assign(service, held);
    const auto deadline_assignment = assign(service, deadline);

    const auto held_a = with_buttons(static_cast<button_mask>(gamepad_button::south));
    events.enqueue(held_assignment.user_index,
        initial_batch(held.connection.target, 71U, held_a, 100U));
    events.enqueue(deadline_assignment.user_index,
        initial_batch(deadline.connection.target, 72U, held_a, 100U));
    service.pump_events(held_assignment.user_index, focus_state.now());
    service.pump_events(deadline_assignment.user_index, focus_state.now());

    events.enqueue(held_assignment.user_index,
        empty_checkpoint(held.connection.target, 71U, 1U, 599U));
    service.pump_events(held_assignment.user_index, focus_state.now());
    XINPUT_KEYSTROKE output{};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, held_assignment.user_index, 0U, &output) ==
          ERROR_EMPTY);

    events.enqueue(held_assignment.user_index,
        empty_checkpoint(held.connection.target, 71U, 1U, 600U));
    const auto reads_before_unpumped_checkpoint = events.read_calls();
    output.HidCode = 0xA6U;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, held_assignment.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0xA6U);
    CHECK(events.read_calls() == reads_before_unpumped_checkpoint);
    service.pump_events(held_assignment.user_index, focus_state.now());
    CHECK(service.get_keystroke(xinput::flavor::v1_4, held_assignment.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(output.Flags == static_cast<WORD>(XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));

    events.enqueue(deadline_assignment.user_index,
        history_events(deadline.connection.target,
            event_stream_id{72U},
            2U,
            {{{}, 600U}},
            {},
            600U));
    const auto reads_before_unpumped_get = events.read_calls();
    output.HidCode = 0xA5U;
    CHECK(
        service.get_keystroke(xinput::flavor::v1_4, deadline_assignment.user_index, 0U, &output) ==
        ERROR_EMPTY);
    CHECK(output.HidCode == 0xA5U);
    CHECK(events.read_calls() == reads_before_unpumped_get);
    service.pump_events(deadline_assignment.user_index, focus_state.now());
    output = {};
    CHECK(
        service.get_keystroke(xinput::flavor::v1_4, deadline_assignment.user_index, 0U, &output) ==
        ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYUP);
}

TEST_CASE("continuous foreground epoch preserves queued keys beyond the snapshot lease",
    "[component][xinput][events][focus]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 81U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{81U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());

    const auto completed = focus_state.published().completed_observations;
    focus_state.advance(std::chrono::milliseconds{200});
    focus_state.wait_for_refresh_after(completed);
    focus_state.advance(std::chrono::milliseconds{60});
    XINPUT_KEYSTROKE output{};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);

    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{81U},
            3U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::east)), 300U}},
            {},
            300U));
    service.pump_events(occupant.user_index, focus_state.now());
    focus_state.set(focus::evidence::background);
    service.pump_events(occupant.user_index, focus_state.now());
    focus_state.set(focus::evidence::foreground);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 82U, {}, 400U));
    service.pump_events(occupant.user_index, focus_state.now());
    output.HidCode = 0xEFU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0xEFU);
}

TEST_CASE("blocked history read cannot commit through an expired captured focus lease",
    "[component][xinput][events][focus][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 91U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{91U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            {},
            200U));

    events.block_next_read();
    std::jthread pump([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));

    const auto before_refresh = focus_state.published().completed_observations;
    focus_state.advance(std::chrono::milliseconds{200});
    focus_state.wait_for_refresh_after(before_refresh);
    focus_state.advance(std::chrono::milliseconds{60});
    events.release_read();
    pump.join();

    service.pump_events(occupant.user_index, focus_state.now());
    XINPUT_KEYSTROKE output{};
    output.HidCode = 0xFEU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0xFEU);
    CHECK(events.close_calls() == 1U);
    CHECK(events.open_calls() == 2U);
    CHECK(events.is_open(occupant.user_index));
}

TEST_CASE("disable then enable revokes an event read already in flight",
    "[component][xinput][events][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 96U, {}, 100U));
    service.pump_events(occupant.user_index, focus_state.now());
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{96U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            {},
            200U));

    events.block_next_read();
    std::jthread pump([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));
    service.enable(FALSE);
    service.enable(TRUE);
    events.release_read();
    pump.join();

    XINPUT_KEYSTROKE output{};
    output.HidCode = 0x0DU;
    CHECK(service.get_keystroke(xinput::flavor::v1_4, occupant.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(output.HidCode == 0x0DU);
}

TEST_CASE("stale blocked open closes without reading or revoking a fresh Guide ticket",
    "[component][xinput][events][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 97U, {}, 100U));

    events.block_next_open();
    std::jthread opener([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_open(std::chrono::seconds{2}));

    service.enable(FALSE);
    service.enable(TRUE);
    service.pump_events(occupant.user_index, focus_state.now());
    const auto ticket = service.begin_guide_wait(occupant.user_index);
    REQUIRE(ticket.has_value());
    CHECK(events.open_calls() == 1U);

    events.release_open();
    opener.join();

    CHECK(events.read_calls() == 0U);
    CHECK(events.close_calls() == 1U);
    CHECK_FALSE(events.is_open(occupant.user_index));
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));

    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(events.open_calls() == 2U);
    CHECK(events.read_calls() == 1U);
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{97U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::guide)), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(service.wait_for_guide_button(*ticket).has_value());
}

TEST_CASE("stale blocked open failure preserves a usable fresh Guide ticket",
    "[component][xinput][events][guide][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);
    events.enqueue(occupant.user_index, initial_batch(connection.connection.target, 98U, {}, 100U));
    events.fail_next_open(history_failure{.code = history_error::provider_failure});
    events.block_next_open();

    std::jthread opener([&] { service.pump_events(occupant.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_open(std::chrono::seconds{2}));

    service.enable(FALSE);
    service.enable(TRUE);
    service.pump_events(occupant.user_index, focus_state.now());
    const auto ticket = service.begin_guide_wait(occupant.user_index);
    REQUIRE(ticket.has_value());
    CHECK(events.open_calls() == 1U);

    events.release_open();
    opener.join();

    CHECK(events.read_calls() == 0U);
    CHECK(events.close_calls() == 0U);
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service, occupant.user_index));

    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(events.open_calls() == 2U);
    events.enqueue(occupant.user_index,
        history_events(connection.connection.target,
            event_stream_id{98U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::guide)), 200U}},
            {},
            200U));
    service.pump_events(occupant.user_index, focus_state.now());
    CHECK(service.wait_for_guide_button(*ticket).has_value());
}

TEST_CASE("ANY retrieves prepared keys without advancing pending Guide history",
    "[component][xinput][events][guide]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);

    events.enqueue(first_assignment.user_index,
        initial_batch(first.connection.target, 99U, {}, 100U));
    events.enqueue(second_assignment.user_index,
        initial_batch(second.connection.target, 100U, {}, 100U));
    service.pump_events(first_assignment.user_index, focus_state.now());
    service.pump_events(second_assignment.user_index, focus_state.now());
    events.enqueue(first_assignment.user_index,
        history_events(first.connection.target,
            event_stream_id{99U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            {},
            200U));
    events.enqueue(second_assignment.user_index,
        history_events(second.connection.target,
            event_stream_id{100U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::east) |
                           static_cast<button_mask>(gamepad_button::guide)),
                200U}},
            {},
            200U));

    const auto ticket = service.begin_guide_wait(second_assignment.user_index);
    REQUIRE(ticket.has_value());
    const auto opens_before_retrieval = events.open_calls();
    const auto reads_before_retrieval = events.read_calls();
    const auto closes_before_retrieval = events.close_calls();
    XINPUT_KEYSTROKE output{};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, first_assignment.user_index, 0U, &output) ==
          ERROR_EMPTY);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, any_user, 0U, &output) == ERROR_EMPTY);
    CHECK(events.open_calls() == opens_before_retrieval);
    CHECK(events.read_calls() == reads_before_retrieval);
    CHECK(events.close_calls() == closes_before_retrieval);
    CHECK(xinput::detail::service_test_access::guide_wait_pending(service,
        second_assignment.user_index));

    service.pump_events(first_assignment.user_index, focus_state.now());
    service.pump_events(second_assignment.user_index, focus_state.now());
    CHECK(service.get_keystroke(xinput::flavor::v1_4, first_assignment.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5800U);
    CHECK(service.get_keystroke(xinput::flavor::v1_4, any_user, 0U, &output) == ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5801U);
    const auto cancellation = service.cancel_guide_wait(*ticket);
    REQUIRE(cancellation.has_value());
    CHECK_FALSE(*cancellation);
    CHECK(service.wait_for_guide_button(*ticket).has_value());
}

TEST_CASE("blocked reader operation does not hold the presentation gate or another position",
    "[component][xinput][events][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto second = devices.add();
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);
    events.enqueue(first_assignment.user_index,
        initial_batch(first.connection.target, 101U, {}, 100U));
    events.enqueue(second_assignment.user_index,
        initial_batch(second.connection.target, 102U, {}, 100U));
    service.pump_events(first_assignment.user_index, focus_state.now());
    service.pump_events(second_assignment.user_index, focus_state.now());
    events.enqueue(first_assignment.user_index,
        history_events(first.connection.target,
            event_stream_id{101U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::south)), 200U}},
            {},
            200U));
    events.enqueue(second_assignment.user_index,
        history_events(second.connection.target,
            event_stream_id{102U},
            2U,
            {{with_buttons(static_cast<button_mask>(gamepad_button::east)), 200U}},
            {},
            200U));

    events.block_next_read();
    std::jthread blocked_pump(
        [&] { service.pump_events(first_assignment.user_index, focus_state.now()); });
    std::future<DWORD> state_result;
    std::future<void> other_pump;
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_read(std::chrono::seconds{2}));
    state_result = std::async(std::launch::async, [&] {
        XINPUT_STATE state{};
        return service.get_state(xinput::flavor::v1_4, first_assignment.user_index, &state);
    });
    other_pump = std::async(std::launch::async,
        [&] { service.pump_events(second_assignment.user_index, focus_state.now()); });
    const bool state_ready =
        state_result.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    CHECK(state_ready);
    if (state_ready) {
        CHECK(state_result.get() == ERROR_SUCCESS);
    }
    const bool other_ready =
        other_pump.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
    CHECK(other_ready);
    if (other_ready) {
        other_pump.get();
    }
    CHECK(xinput::detail::service_test_access::event_operation_reserved(service,
        first_assignment.user_index));
    const auto reads_before = events.read_calls();
    service.pump_events(first_assignment.user_index, focus_state.now());
    CHECK(events.read_calls() == reads_before);
    const auto before = focus_state.published().completed_observations;
    CHECK(wait_until([&] { return focus_state.published().completed_observations > before; }));

    events.release_read();
    blocked_pump.join();
    XINPUT_KEYSTROKE output{};
    CHECK(service.get_keystroke(xinput::flavor::v1_4, second_assignment.user_index, 0U, &output) ==
          ERROR_SUCCESS);
    CHECK(output.VirtualKey == 0x5801U);
}

TEST_CASE("stale open reservation closes its exact reader before a replacement can open",
    "[component][xinput][events][concurrency]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto replacement = devices.add();
    const auto original_assignment = assign(service, first);
    events.enqueue(original_assignment.user_index,
        initial_batch(first.connection.target, 111U, {}, 100U));
    events.block_next_open();
    std::jthread opener(
        [&] { service.pump_events(original_assignment.user_index, focus_state.now()); });
    const release_event_source_on_exit release{events};
    REQUIRE(events.wait_for_open(std::chrono::seconds{2}));

    REQUIRE(service.unassign(original_assignment).has_value());
    const auto replacement_assignment = assign(service, replacement);
    XINPUT_STATE state{};
    CHECK(service.get_state(xinput::flavor::v1_4, replacement_assignment.user_index, &state) ==
          ERROR_SUCCESS);
    service.pump_events(replacement_assignment.user_index, focus_state.now());
    CHECK(events.open_calls() == 1U);

    events.release_open();
    opener.join();
    const auto closed = events.last_closed();
    if (!closed.has_value()) {
        FAIL("the stale reader must close before the replacement can open");
    }
    CHECK(closed->target == first.connection.target);
    CHECK_FALSE(events.is_open(replacement_assignment.user_index));
    events.enqueue(replacement_assignment.user_index,
        initial_batch(replacement.connection.target, 112U, {}, 200U));
    service.pump_events(replacement_assignment.user_index, focus_state.now());
    CHECK(events.open_calls() == 2U);
}

TEST_CASE("failed close retains the old handle and prevents replacement",
    "[component][xinput][events][recovery]") {
    manual_focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    controlled_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, qpc_frequency};
    const auto first = devices.add();
    const auto replacement = devices.add();
    const auto original_assignment = assign(service, first);
    events.enqueue(original_assignment.user_index,
        initial_batch(first.connection.target, 121U, {}, 100U));
    service.pump_events(original_assignment.user_index, focus_state.now());
    const auto original_reader = xinput::detail::service_test_access::event_reader(service, 0U);
    REQUIRE(original_reader.has_value());

    events.fail_close_attempts(2U);
    REQUIRE(service.unassign(original_assignment).has_value());
    CHECK(xinput::detail::service_test_access::event_reader(service, 0U) == original_reader);
    const auto replacement_assignment = assign(service, replacement);
    events.enqueue(replacement_assignment.user_index,
        initial_batch(replacement.connection.target, 122U, {}, 200U));
    service.pump_events(replacement_assignment.user_index, focus_state.now());
    CHECK(events.open_calls() == 1U);
    CHECK(events.last_closed() == original_reader);
    CHECK_FALSE(events.is_open(0U));

    service.pump_events(replacement_assignment.user_index, focus_state.now());
    CHECK(events.open_calls() == 2U);
    CHECK(events.is_open(0U));
}
