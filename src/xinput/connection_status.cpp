#include <inputnext/xinput.hpp>

#include "service_state.hpp"
#include <inputnext/input_binding.hpp>

#include <optional>

namespace inputnext::xinput {

DWORD service::connection_status(DWORD user_index) {
    if (user_index >= implementation_->positions.size()) {
        return ERROR_BAD_ARGUMENTS;
    }

    std::optional<read_pin> pin;
    const auto guard = implementation_->focus.lock_presentation();
    if (implementation_->closed) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    auto& position = implementation_->positions[user_index];
    if (!position.occupant.has_value() ||
        !implementation::live_binding_matches(position, *position.occupant, pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return ERROR_SUCCESS;
}

} // namespace inputnext::xinput
