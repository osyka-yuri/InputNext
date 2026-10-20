#include "service_test_helpers.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <latch>
#include <thread>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

TEST_CASE("presentation packets use canonical translated state across Guide views and reset on "
          "reassignment",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::guide)});
    const auto occupant = assign(service, connection);

    service.enable(FALSE);
    XINPUT_STATE state{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    service.enable(TRUE);
    REQUIRE(service.get_state(xinput::flavor::v1_3, occupant.user_index, &state, false) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);

    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state, true) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0x0400U);
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state, true) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    REQUIRE(service.get_state(xinput::flavor::v1_3, occupant.user_index, &state, false) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);

    service.enable(FALSE);
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == 0U);
    service.enable(TRUE);

    const auto changed = gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south) |
                                                  static_cast<button_mask>(gamepad_button::guide)};
    REQUIRE(publish(connection, changed));
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state, false) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 2U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);

    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)}));
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state, false) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 3U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);

    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south) |
                                 static_cast<button_mask>(gamepad_button::guide)}));
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state, false) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 4U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);

    const auto duplicate =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(duplicate.has_value());
    CHECK(*duplicate == occupant);
    CHECK(packet_for(service, occupant.user_index) == 4U);

    REQUIRE(service.unassign(occupant).has_value());
    const auto reassigned = assign(service, connection);
    CHECK(reassigned.user_index == occupant.user_index);
    CHECK(reassigned.id != occupant.id);
    REQUIRE(
        service.get_state(xinput::flavor::v1_4, reassigned.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
}

TEST_CASE("a late older XInput read returns the newest committed state and packet",
    "[component][xinput][concurrency]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);

    xinput::detail::service_test_access::prepared_read older{};
    REQUIRE(
        xinput::detail::service_test_access::prepare(service, occupant.user_index, false, older) ==
        ERROR_SUCCESS);
    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}));

    XINPUT_STATE newer{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &newer, true) ==
            ERROR_SUCCESS);
    CHECK(newer.dwPacketNumber == 1U);
    CHECK(newer.Gamepad.wButtons == XINPUT_GAMEPAD_B);

    XINPUT_STATE late{};
    REQUIRE(xinput::detail::service_test_access::commit(service, older, &late) == ERROR_SUCCESS);
    CHECK(late.dwPacketNumber == newer.dwPacketNumber);
    CHECK(late.Gamepad.wButtons == newer.Gamepad.wButtons);

    XINPUT_STATE final{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &final) == ERROR_SUCCESS);
    CHECK(final.dwPacketNumber == newer.dwPacketNumber);
    CHECK(final.Gamepad.wButtons == newer.Gamepad.wButtons);
}

TEST_CASE("a newer equal state fences a prepared older state without changing its packet",
    "[component][xinput][concurrency]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant = assign(service, connection);

    XINPUT_STATE initial{};
    REQUIRE(
        service.get_state(xinput::flavor::v1_4, occupant.user_index, &initial) == ERROR_SUCCESS);
    REQUIRE(initial.dwPacketNumber == 1U);
    REQUIRE(initial.Gamepad.wButtons == XINPUT_GAMEPAD_A);

    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}));
    xinput::detail::service_test_access::prepared_read older_different_state{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                older_different_state) == ERROR_SUCCESS);

    REQUIRE(publish(connection,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)}));
    XINPUT_STATE newer_equal_state{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &newer_equal_state) ==
            ERROR_SUCCESS);
    CHECK(newer_equal_state.dwPacketNumber == initial.dwPacketNumber);
    CHECK(newer_equal_state.Gamepad.wButtons == initial.Gamepad.wButtons);

    XINPUT_STATE late{};
    REQUIRE(xinput::detail::service_test_access::commit(service, older_different_state, &late) ==
            ERROR_SUCCESS);
    CHECK(late.dwPacketNumber == initial.dwPacketNumber);
    CHECK(late.Gamepad.wButtons == initial.Gamepad.wButtons);

    XINPUT_STATE final{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &final) == ERROR_SUCCESS);
    CHECK(final.dwPacketNumber == initial.dwPacketNumber);
    CHECK(final.Gamepad.wButtons == initial.Gamepad.wButtons);
}

TEST_CASE("two ready readers preserve coherent state and packets under concurrent access",
    "[component][xinput][concurrency]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const gamepad_state first_sample{
        .buttons = static_cast<button_mask>(gamepad_button::south),
        .left_stick = {.x = 1.0F, .y = -1.0F},
        .right_stick = {.x = -1.0F, .y = 1.0F},
        .left_trigger = 1.0F,
        .right_trigger = 0.5F,
    };
    const gamepad_state second_sample{
        .buttons = static_cast<button_mask>(gamepad_button::east),
        .left_stick = {.x = -1.0F, .y = 1.0F},
        .right_stick = {.x = 1.0F, .y = -1.0F},
        .left_trigger = 0.5F,
        .right_trigger = 1.0F,
    };
    const auto first_connection = devices.add(first_sample);
    const auto second_connection = devices.add(second_sample);
    const auto first_assignment = assign(service, first_connection);
    const auto second_assignment = assign(service, second_connection);

    struct read_outcome final {
        DWORD status{};
        XINPUT_STATE state{};
    };

    constexpr std::size_t read_count = 8U;
    std::array<read_outcome, read_count> first_reads{};
    std::array<read_outcome, read_count> second_reads{};
    std::latch start_gate{1};
    std::jthread first_worker;
    std::jthread second_worker;
    std::promise<void> first_done;
    std::promise<void> second_done;
    auto first_finished = first_done.get_future();
    auto second_finished = second_done.get_future();

    struct reader_cleanup final {
        std::latch& start_gate;
        std::jthread& first_worker;
        std::jthread& second_worker;
        bool released{};

        reader_cleanup(std::latch& start, std::jthread& first, std::jthread& second) noexcept
            : start_gate(start),
              first_worker(first),
              second_worker(second) {}

        void release_start() noexcept {
            if (!released) {
                start_gate.count_down();
                released = true;
            }
        }

        void join() noexcept {
            if (first_worker.joinable()) {
                first_worker.join();
            }
            if (second_worker.joinable()) {
                second_worker.join();
            }
        }

        reader_cleanup(const reader_cleanup&) = delete;
        reader_cleanup& operator=(const reader_cleanup&) = delete;
        reader_cleanup(reader_cleanup&&) = delete;
        reader_cleanup& operator=(reader_cleanup&&) = delete;

        ~reader_cleanup() {
            release_start();
            join();
        }
    } cleanup{start_gate, first_worker, second_worker};

    first_worker = std::jthread{[&] {
        start_gate.wait();
        for (auto& result : first_reads) {
            result.status =
                service.get_state(xinput::flavor::v1_4, first_assignment.user_index, &result.state);
        }
        first_done.set_value();
    }};
    second_worker = std::jthread{[&] {
        start_gate.wait();
        for (auto& result : second_reads) {
            result.status = service.get_state(xinput::flavor::v1_4,
                second_assignment.user_index,
                &result.state);
        }
        second_done.set_value();
    }};

    cleanup.release_start();
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    const bool first_completed =
        first_finished.wait_until(completion_deadline) == std::future_status::ready;
    const bool second_completed =
        second_finished.wait_until(completion_deadline) == std::future_status::ready;
    cleanup.join();
    first_finished.get();
    second_finished.get();
    CHECK(first_completed);
    CHECK(second_completed);

    for (const auto& result : first_reads) {
        CHECK(result.status == ERROR_SUCCESS);
        CHECK(result.state.dwPacketNumber == 1U);
        CHECK(result.state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
        CHECK(result.state.Gamepad.bLeftTrigger == 255U);
        CHECK(result.state.Gamepad.bRightTrigger == 127U);
        CHECK(result.state.Gamepad.sThumbLX == 32767);
        CHECK(result.state.Gamepad.sThumbLY == -32768);
        CHECK(result.state.Gamepad.sThumbRX == -32768);
        CHECK(result.state.Gamepad.sThumbRY == 32767);
    }
    for (const auto& result : second_reads) {
        CHECK(result.status == ERROR_SUCCESS);
        CHECK(result.state.dwPacketNumber == 1U);
        CHECK(result.state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
        CHECK(result.state.Gamepad.bLeftTrigger == 127U);
        CHECK(result.state.Gamepad.bRightTrigger == 255U);
        CHECK(result.state.Gamepad.sThumbLX == -32768);
        CHECK(result.state.Gamepad.sThumbLY == 32767);
        CHECK(result.state.Gamepad.sThumbRX == 32767);
        CHECK(result.state.Gamepad.sThumbRY == -32768);
    }
}
