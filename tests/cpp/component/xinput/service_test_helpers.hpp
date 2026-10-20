#pragma once

#include "support/xinput/test_ports.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <stdexcept>

namespace inputnext::xinput_test_support {

[[nodiscard]] inline xinput::assignment assign(xinput::service& service,
    const test_connection& connection) {
    const auto assigned =
        service.assign(connection.connection.target, connection.connection.reader);
    if (!assigned.has_value()) {
        throw std::runtime_error{"test XInput assignment failed"};
    }
    return *assigned;
}

[[nodiscard]] inline bool publish(test_connection const& connection, gamepad_state state) {
    return connection.connection.producer
        .publish(input_update{.state = state, .source_timestamp_us = 2U, .observation_qpc = 2U})
        .has_value();
}

[[nodiscard]] inline std::uint32_t
packet_for(xinput::service& service, DWORD index, DWORD expected_status = ERROR_SUCCESS) {
    XINPUT_STATE state{};
    if (service.get_state(xinput::flavor::v1_4, index, &state) != expected_status) {
        throw std::runtime_error{"unexpected XInput state status"};
    }
    return state.dwPacketNumber;
}

} // namespace inputnext::xinput_test_support
