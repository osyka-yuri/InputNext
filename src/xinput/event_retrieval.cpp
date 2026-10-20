#include <inputnext/xinput.hpp>

#include "service_state.hpp"
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace inputnext::xinput {

bool service::implementation::pop_eligibility(focus::observer::presentation_guard& guard,
    position_record& position,
    const assignment& occupant,
    std::optional<read_pin>& pin,
    guide_wait_state& failure) const noexcept {
    auto captured = position.reader.capture();
    if (!captured.has_value()) {
        failure = guide_wait_state::not_connected;
        return false;
    }
    pin.emplace(std::move(*captured));
    if (pin->target() != occupant.target) {
        failure = guide_wait_state::not_connected;
        return false;
    }
    if (event_identity_exhausted) {
        failure = guide_wait_state::identity_exhausted;
        return false;
    }
    const auto snapshot = guard.published();
    const auto epoch = guard.current_foreground_epoch();
    if (!enabled || !epoch.has_value()) {
        failure = guide_wait_state::ineligible;
        return false;
    }
    if (!guard.matches_foreground(snapshot) || !guard.matches_foreground_epoch(*epoch) ||
        !event_qualification_is_current(position.events, occupant, *epoch)) {
        failure = guide_wait_state::ineligible;
        return false;
    }
    return true;
}

bool service::implementation::try_pop_event(std::uint32_t user_index,
    XINPUT_KEYSTROKE& output,
    bool& connected) noexcept {
    if (user_index >= 4U) {
        connected = false;
        return false;
    }

    std::optional<read_pin> live_pin;
    XINPUT_KEYSTROKE staged_output{};
    bool success{};
    bool notify{};
    {
        auto guard = focus.lock_presentation();
        auto& position = positions[user_index];
        auto& events = position.events;
        if (closed || !position.occupant.has_value()) {
            connected = false;
            const auto reason = closed ? guide_wait_state::closed : guide_wait_state::not_connected;
            notify = invalidate_event_position(position, reason);
        } else {
            const auto occupant = *position.occupant;
            auto failure = guide_wait_state::ineligible;
            if (!pop_eligibility(guard, position, occupant, live_pin, failure)) {
                connected = failure != guide_wait_state::not_connected;
                notify = invalidate_event_position(position, failure);
            } else {
                connected = true;
                success = events.tracker.pop(user_index, staged_output);
                if (success) {
                    last_keystroke_position = user_index;
                    output = staged_output;
                }
            }
        }
    }
    if (notify) {
        notify_guide_waiters();
    }
    return success;
}

DWORD service::get_keystroke(flavor version,
    DWORD user_index,
    DWORD /*reserved*/,
    XINPUT_KEYSTROKE* keystroke) {
    if (version == flavor::v9_1_0) {
        return ERROR_NOT_SUPPORTED;
    }
    constexpr DWORD any_user = 0x000000FFU;
    if (keystroke == nullptr || (user_index >= 4U && user_index != any_user)) {
        return ERROR_BAD_ARGUMENTS;
    }

    if (user_index == any_user) {
        std::uint32_t start{};
        {
            const auto guard = implementation_->focus.lock_presentation();
            start = implementation_->last_keystroke_position.has_value()
                        ? (*implementation_->last_keystroke_position + 1U) % 4U
                        : 0U;
        }
        bool any_connected{};
        for (std::uint32_t offset = 0U; offset < 4U; ++offset) {
            const auto position = (start + offset) % 4U;
            XINPUT_KEYSTROKE staged_output{};
            bool connected{};
            if (implementation_->try_pop_event(position, staged_output, connected)) {
                *keystroke = staged_output;
                return ERROR_SUCCESS;
            }
            any_connected = connected || any_connected;
        }
        return any_connected ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
    }

    XINPUT_KEYSTROKE staged_output{};
    bool connected{};
    if (implementation_->try_pop_event(user_index, staged_output, connected)) {
        *keystroke = staged_output;
        return ERROR_SUCCESS;
    }
    return connected ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
}

} // namespace inputnext::xinput
