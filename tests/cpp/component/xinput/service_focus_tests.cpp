#include "focus/observer_test_access.hpp"
#include "service_test_helpers.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

TEST_CASE("a blocked focus probe leaves ready reads independent and rejects stale focus on commit",
    "[component][xinput][concurrency]") {
    registry_fixture devices;
    metadata_source metadata;
    test::focus::manual_clock clock;
    const focus::process_instance process{.pid = 2234U, .creation_time_100ns = 6678U};
    const std::vector<focus::reading> readings{
        {.process = process, .state = focus::evidence::foreground},
        {.process = process, .state = focus::evidence::foreground},
        {.process = process, .state = focus::evidence::background},
        {.process = process, .state = focus::evidence::foreground},
        {.process = process, .state = focus::evidence::foreground},
    };
    auto controlled = std::make_unique<test::focus::controlled_source>(process, readings, 2U);
    auto* const source = controlled.get();
    const auto observer_owner =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x22U),
            std::move(controlled),
            &test::focus::manual_clock::read,
            &clock);
    auto& observer = *observer_owner;

    struct release_guard final {
        explicit release_guard(test::focus::controlled_source& source_value) noexcept
            : source(source_value) {}

        test::focus::controlled_source& source;

        ~release_guard() { source.release(); }

        release_guard(const release_guard&) = delete;
        release_guard& operator=(const release_guard&) = delete;
        release_guard(release_guard&&) = delete;
        release_guard& operator=(release_guard&&) = delete;
    };

    const release_guard source_release{*source};

    const bool blocked = source->wait_until_blocked();
    const auto initial_focus = observer.published();
    REQUIRE(blocked);
    REQUIRE(initial_focus.state == focus::evidence::foreground);

    xinput::service service{observer, metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);

    struct read_outcome final {
        DWORD status{};
        XINPUT_STATE state{};
    };

    std::promise<read_outcome> read_result;
    auto read_future = read_result.get_future();
    std::jthread read_worker([&] {
        read_outcome outcome{};
        outcome.status =
            service.get_state(xinput::flavor::v1_4, occupant.user_index, &outcome.state);
        read_result.set_value(outcome);
    });

    struct release_read_guard final {
        explicit release_read_guard(test::focus::controlled_source& source_value) noexcept
            : source(source_value) {}

        test::focus::controlled_source& source;

        ~release_read_guard() { source.release(); }

        release_read_guard(const release_read_guard&) = delete;
        release_read_guard& operator=(const release_read_guard&) = delete;
        release_read_guard(release_read_guard&&) = delete;
        release_read_guard& operator=(release_read_guard&&) = delete;
    };

    const release_read_guard read_release{*source};

    const bool completed_while_probe_blocked =
        read_future.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready;

    xinput::detail::service_test_access::prepared_read prepared{};
    const auto prepared_status = completed_while_probe_blocked
                                     ? xinput::detail::service_test_access::prepare(service,
                                           occupant.user_index,
                                           false,
                                           prepared)
                                     : ERROR_DEVICE_NOT_CONNECTED;
    source->release();
    read_worker.join();
    const auto read = read_future.get();
    CHECK(completed_while_probe_blocked);
    CHECK(read.status == ERROR_SUCCESS);
    CHECK(read.state.dwPacketNumber == 1U);
    CHECK(read.state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    REQUIRE(completed_while_probe_blocked);
    REQUIRE(prepared_status == ERROR_SUCCESS);

    const bool returned_through_foreground = source->wait_for_returned(4U);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto current_focus = observer.published();
    while (std::chrono::steady_clock::now() < deadline &&
           (current_focus.revision <= initial_focus.revision ||
               current_focus.state != focus::evidence::foreground)) {
        std::this_thread::yield();
        current_focus = observer.published();
    }
    CHECK(returned_through_foreground);
    CHECK(current_focus.revision > initial_focus.revision);
    CHECK(current_focus.state == focus::evidence::foreground);

    XINPUT_STATE state{};
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
}

TEST_CASE("a captured foreground lease that expires before commit stays neutral after recovery",
    "[component][xinput][focus]") {
    test::focus::manual_clock clock;
    const focus::process_instance process{.pid = 3234U, .creation_time_100ns = 7678U};
    const std::vector<focus::reading> readings{
        {.process = process, .state = focus::evidence::foreground, .native_code = 1U},
        {.process = process, .state = focus::evidence::foreground, .native_code = 2U},
    };
    auto controlled = std::make_unique<test::focus::controlled_source>(process, readings, 2U);
    auto* const source = controlled.get();
    const auto observer =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x23U),
            std::move(controlled),
            &test::focus::manual_clock::read,
            &clock);

    struct source_release_guard final {
        explicit source_release_guard(test::focus::controlled_source& source_value) noexcept
            : source(source_value) {}

        test::focus::controlled_source& source;

        ~source_release_guard() { source.release(); }

        source_release_guard(const source_release_guard&) = delete;
        source_release_guard& operator=(const source_release_guard&) = delete;
        source_release_guard(source_release_guard&&) = delete;
        source_release_guard& operator=(source_release_guard&&) = delete;
    };

    const source_release_guard release_source{*source};

    REQUIRE(source->wait_until_blocked());
    const auto captured_focus = observer->published();
    REQUIRE(captured_focus.state == focus::evidence::foreground);
    REQUIRE(captured_focus.revision == 1U);
    REQUIRE(captured_focus.observation_started == clock.now());

    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{*observer, metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);
    xinput::detail::service_test_access::prepared_read captured{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                captured) == ERROR_SUCCESS);

    clock.advance(std::chrono::milliseconds{250});
    XINPUT_STATE state{};
    REQUIRE(
        xinput::detail::service_test_access::commit(service, captured, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);
    const auto expired_focus = observer->published();
    CHECK(expired_focus.revision > captured_focus.revision);
    CHECK(expired_focus.state == focus::evidence::unavailable);

    source->release();
    const bool returned_after_expiry = source->wait_for_returned(3U);
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto recovered_focus = observer->published();
    while (std::chrono::steady_clock::now() < recovery_deadline &&
           (recovered_focus.revision <= expired_focus.revision ||
               recovered_focus.state != focus::evidence::foreground)) {
        std::this_thread::yield();
        recovered_focus = observer->published();
    }
    CHECK(returned_after_expiry);
    REQUIRE(recovered_focus.revision > expired_focus.revision);
    REQUIRE(recovered_focus.state == focus::evidence::foreground);
    CHECK(recovered_focus.observation_started == clock.now());

    state = {};
    REQUIRE(
        xinput::detail::service_test_access::commit(service, captured, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    observer->stop();
}

TEST_CASE("captured neutral stays neutral while active enable is rechecked after the read phase",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);

    focus_state.set(focus::evidence::background);
    xinput::detail::service_test_access::prepared_read neutral{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                neutral) == ERROR_SUCCESS);
    focus_state.set(focus::evidence::foreground);
    XINPUT_STATE state{};
    REQUIRE(xinput::detail::service_test_access::commit(service, neutral, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    xinput::detail::service_test_access::prepared_read active{};
    REQUIRE(
        xinput::detail::service_test_access::prepare(service, occupant.user_index, false, active) ==
        ERROR_SUCCESS);
    service.enable(FALSE);
    service.enable(TRUE);
    REQUIRE(xinput::detail::service_test_access::commit(service, active, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);

    const XINPUT_GAMEPAD previous{};
    REQUIRE(xinput::detail::service_test_access::seed_packet(service,
        occupant,
        std::numeric_limits<std::uint32_t>::max(),
        previous));
    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}));
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
}

TEST_CASE("an active captured read stays neutral when disabled through commit",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);

    XINPUT_STATE state{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    REQUIRE(state.dwPacketNumber == 1U);
    REQUIRE(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}));

    xinput::detail::service_test_access::prepared_read active{};
    REQUIRE(
        xinput::detail::service_test_access::prepare(service, occupant.user_index, false, active) ==
        ERROR_SUCCESS);
    service.enable(FALSE);

    REQUIRE(xinput::detail::service_test_access::commit(service, active, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);
    CHECK(state.Gamepad.bLeftTrigger == 0U);
    CHECK(state.Gamepad.bRightTrigger == 0U);
    CHECK(state.Gamepad.sThumbLX == 0);
    CHECK(state.Gamepad.sThumbLY == 0);
    CHECK(state.Gamepad.sThumbRX == 0);
    CHECK(state.Gamepad.sThumbRY == 0);

    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);
}
