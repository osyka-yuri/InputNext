#include "service_test_helpers.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/source_lifetime.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

TEST_CASE("assignment identities are exact, nonreused, and exhaustion preserves positions",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(),
        metadata,
        {.next_assignment = std::numeric_limits<std::uint64_t>::max()}};
    const auto first = devices.add();
    const auto second = devices.add();

    const auto final_id = assign(service, first);
    CHECK(final_id.id == std::numeric_limits<std::uint64_t>::max());
    const auto duplicate = service.assign(first.connection.target, first.connection.reader);
    REQUIRE(duplicate.has_value());
    CHECK(*duplicate == final_id);

    const auto exhausted = service.assign(second.connection.target, second.connection.reader);
    REQUIRE(exhausted.has_value() == false);
    CHECK(exhausted.error() == error::identity_exhausted);
    const auto positions = service.positions();
    CHECK(positions[final_id.user_index] == final_id);
    CHECK(positions[1U].has_value() == false);

    REQUIRE(service.unassign(final_id).has_value());
    const auto exhausted_after_release =
        service.assign(second.connection.target, second.connection.reader);
    REQUIRE(exhausted_after_release.has_value() == false);
    CHECK(exhausted_after_release.error() == error::identity_exhausted);
    CHECK(service.positions()[0U].has_value() == false);
}

TEST_CASE("four presentation positions use lowest free slots and reject stale release tokens",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    std::array<test_connection, 5U> connections{};
    std::array<xinput::assignment, 4U> assigned{};
    for (auto& connection : connections) {
        connection = devices.add();
    }
    for (std::size_t index = 0U; index < assigned.size(); ++index) {
        assigned[index] = assign(service, connections[index]);
        CHECK(assigned[index].user_index == index);
    }

    auto full =
        service.assign(connections[4U].connection.target, connections[4U].connection.reader);
    REQUIRE(full.has_value() == false);
    CHECK(full.error() == error::capacity_exceeded);

    REQUIRE(service.unassign(assigned[1U]).has_value());
    const auto fifth = assign(service, connections[4U]);
    CHECK(fifth.user_index == assigned[1U].user_index);
    CHECK(fifth.id != assigned[1U].id);
    const auto stale_release = service.unassign(assigned[1U]);
    REQUIRE(stale_release.has_value() == false);
    CHECK(stale_release.error() == error::not_connected);
    CHECK(service.positions()[fifth.user_index] == fifth);
    CHECK(service.positions()[0U] == assigned[0U]);
    CHECK(service.positions()[2U] == assigned[2U]);
    CHECK(service.positions()[3U] == assigned[3U]);
}

TEST_CASE("a physical reconnect receives a fresh assignment after the old connection is released",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    auto old_connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto old_assignment = assign(service, old_connection);

    std::unique_ptr<source_lifetime> candidate = std::make_unique<test_source_lifetime>();
    auto replacement_result =
        devices.registry().replace(old_connection.connection.target, candidate);
    REQUIRE(replacement_result.has_value());
    test_connection replacement{.connection = *replacement_result};
    CHECK(replacement.connection.target.device == old_connection.connection.target.device);
    CHECK(replacement.connection.target.connection != old_connection.connection.target.connection);
    REQUIRE(publish(replacement,
        gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)}));

    REQUIRE(service.unassign(old_assignment).has_value());
    const auto new_assignment = assign(service, replacement);
    CHECK(new_assignment.user_index == old_assignment.user_index);
    CHECK(new_assignment.target != old_assignment.target);
    CHECK(new_assignment.id != old_assignment.id);
    const auto stale_release = service.unassign(old_assignment);
    REQUIRE(stale_release.has_value() == false);
    CHECK(service.positions()[new_assignment.user_index] == new_assignment);

    XINPUT_STATE state{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, new_assignment.user_index, &state) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
}

TEST_CASE("commit rejects replacement and retirement before its exact live linearization point",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto old_connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto old_assignment = assign(service, old_connection);

    xinput::detail::service_test_access::prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                old_assignment.user_index,
                false,
                prepared) == ERROR_SUCCESS);
    REQUIRE(service.unassign(old_assignment).has_value());
    const auto replacement =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)});
    const auto new_assignment = assign(service, replacement);
    REQUIRE(new_assignment.user_index == old_assignment.user_index);

    XINPUT_STATE canary{};
    canary.dwPacketNumber = 0xDEADBEEFU;
    canary.Gamepad.wButtons = 0x1234U;
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &canary) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(canary.dwPacketNumber == 0xDEADBEEFU);
    CHECK(canary.Gamepad.wButtons == 0x1234U);
    CHECK(packet_for(service, new_assignment.user_index) == 1U);

    xinput::detail::service_test_access::prepared_read retired_read{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                new_assignment.user_index,
                false,
                retired_read) == ERROR_SUCCESS);
    REQUIRE(devices.registry().retire(replacement.connection.target).has_value());
    canary.dwPacketNumber = 0xBAD0U;
    canary.Gamepad.wButtons = 0x4321U;
    CHECK(xinput::detail::service_test_access::commit(service, retired_read, &canary) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(canary.dwPacketNumber == 0xBAD0U);
    CHECK(canary.Gamepad.wButtons == 0x4321U);
}
