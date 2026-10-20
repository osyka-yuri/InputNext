#pragma once

#include "xinput/service_state.hpp"
#include <inputnext/xinput.hpp>

#include <cstdint>

namespace inputnext::xinput::detail {

struct service_test_access final {
    using prepared_read = service::implementation::prepared_state_read;

    [[nodiscard]] static DWORD
    prepare(service& instance, DWORD user_index, bool include_guide, prepared_read& prepared) {
        return instance.implementation_->prepare_state_read(user_index, include_guide, prepared);
    }

    [[nodiscard]] static DWORD
    commit(service& instance, const prepared_read& prepared, XINPUT_STATE* output) {
        return instance.implementation_->commit_state_read(prepared, output);
    }

    [[nodiscard]] static bool seed_packet(service& instance,
        const assignment& expected,
        std::uint32_t packet,
        const XINPUT_GAMEPAD& previous) {
        const auto guard = instance.implementation_->focus.lock_presentation();
        if (expected.user_index >= instance.implementation_->positions.size()) {
            return false;
        }
        auto& position = instance.implementation_->positions[expected.user_index];
        if (!position.occupant.has_value() || *position.occupant != expected) {
            return false;
        }
        position.packet_initialized = true;
        position.packet_number = packet;
        position.canonical_gamepad = previous;
        return true;
    }
};

} // namespace inputnext::xinput::detail
