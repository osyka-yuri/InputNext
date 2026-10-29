#include "support/xinput/service_test_access.hpp"

#include "support/xinput/test_ports.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

namespace {

[[nodiscard]] xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto result = service.assign(connection.connection.target, connection.connection.reader);
    if (!result.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *result;
}

} // namespace

TEST_CASE("prepared GetState binding retains its identity through retirement and reassignment",
    "[component][xinput][lifetime]") {
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto first = devices.add();
    const auto first_assignment = assign(service, first);
    xinput::detail::service_test_access::prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                first_assignment.user_index,
                false,
                prepared) == ERROR_SUCCESS);
    REQUIRE(prepared.connection_pin.validate_live().has_value());
    const auto initial_target = prepared.connection_pin.target();
    CHECK(initial_target == first.connection.target);

    REQUIRE(service.unassign(first_assignment).has_value());
    REQUIRE(devices.registry().retire(first.connection.target).has_value());
    const auto second = devices.add();
    const auto second_assignment = assign(service, second);
    CHECK(second_assignment.user_index == first_assignment.user_index);
    CHECK(second_assignment.id != first_assignment.id);
    CHECK(second_assignment.target == second.connection.target);
    CHECK(prepared.occupant == first_assignment);
    const auto retained_target = prepared.connection_pin.target();
    CHECK(retained_target == first.connection.target);
    CHECK(!prepared.connection_pin.validate_live().has_value());

    XINPUT_STATE state{.dwPacketNumber = 0xC0FFEEU, .Gamepad = {}};
    state.Gamepad.wButtons = 0xA55AU;
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == 0xC0FFEEU);
    CHECK(state.Gamepad.wButtons == 0xA55AU);
}

TEST_CASE("GetState preparation follows assignment and control transitions",
    "[component][xinput]") {
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto first_assignment = assign(service, devices.add());
    const auto second_assignment = assign(service, devices.add());
    xinput::detail::service_test_access::prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                first_assignment.user_index,
                false,
                prepared) == ERROR_SUCCESS);
    CHECK(prepared.occupant == first_assignment);
    CHECK(prepared.eligible_at_capture);
    REQUIRE(prepared.connection_pin.validate_live().has_value());
    const auto target = prepared.connection_pin.target();
    CHECK(target == first_assignment.target);

    service.enable(FALSE);
    xinput::detail::service_test_access::prepared_read disabled{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                first_assignment.user_index,
                false,
                disabled) == ERROR_SUCCESS);
    CHECK(disabled.occupant == first_assignment);
    CHECK(!disabled.eligible_at_capture);
    CHECK(disabled.connection_pin.validate_live().has_value());

    REQUIRE(service.unassign(first_assignment).has_value());
    xinput::detail::service_test_access::prepared_read unassigned{};
    CHECK(xinput::detail::service_test_access::prepare(service,
              first_assignment.user_index,
              false,
              unassigned) == ERROR_DEVICE_NOT_CONNECTED);
    xinput::detail::service_test_access::prepared_read retained{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                second_assignment.user_index,
                false,
                retained) == ERROR_SUCCESS);
    CHECK(retained.occupant == second_assignment);
    CHECK(!retained.eligible_at_capture);

    service.close();
    CHECK(xinput::detail::service_test_access::prepare(service,
              second_assignment.user_index,
              false,
              retained) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xinput::detail::service_test_access::prepare(service, 3U, false, retained) ==
          ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("prepared GetState retains initial inactivity and rechecks current Enable",
    "[component][xinput]") {
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto occupant = assign(service,
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)}));

    service.enable(FALSE);
    xinput::detail::service_test_access::prepared_read initially_inactive{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                initially_inactive) == ERROR_SUCCESS);
    CHECK(!initially_inactive.eligible_at_capture);
    CHECK(initially_inactive.connection_pin.validate_live().has_value());
    service.enable(TRUE);
    XINPUT_STATE state{};
    REQUIRE(xinput::detail::service_test_access::commit(service, initially_inactive, &state) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    xinput::detail::service_test_access::prepared_read disabled_at_commit{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                disabled_at_commit) == ERROR_SUCCESS);
    CHECK(disabled_at_commit.eligible_at_capture);
    service.enable(FALSE);
    REQUIRE(xinput::detail::service_test_access::commit(service, disabled_at_commit, &state) ==
            ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);

    service.enable(TRUE);
    xinput::detail::service_test_access::prepared_read enabled_aba{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant.user_index,
                false,
                enabled_aba) == ERROR_SUCCESS);
    CHECK(enabled_aba.eligible_at_capture);
    service.enable(FALSE);
    service.enable(TRUE);
    REQUIRE(
        xinput::detail::service_test_access::commit(service, enabled_aba, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 1U);
    CHECK(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
}

TEST_CASE("unassign and close reject prepared GetState reads without changing outputs",
    "[component][xinput]") {
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto first_assignment = assign(service, connection);
    xinput::detail::service_test_access::prepared_read before_unassign{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                first_assignment.user_index,
                false,
                before_unassign) == ERROR_SUCCESS);
    REQUIRE(service.unassign(first_assignment).has_value());
    XINPUT_STATE state{.dwPacketNumber = 0xC0FFEEU, .Gamepad = {}};
    state.Gamepad.wButtons = 0xA55AU;
    CHECK(xinput::detail::service_test_access::commit(service, before_unassign, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == 0xC0FFEEU);
    CHECK(state.Gamepad.wButtons == 0xA55AU);

    const auto second_assignment = assign(service, connection);
    xinput::detail::service_test_access::prepared_read before_close{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                second_assignment.user_index,
                false,
                before_close) == ERROR_SUCCESS);
    service.close();
    CHECK(xinput::detail::service_test_access::commit(service, before_close, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == 0xC0FFEEU);
    CHECK(state.Gamepad.wButtons == 0xA55AU);
}
