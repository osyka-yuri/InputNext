#include <inputnext/output.hpp>

#include "output/xinput/rumble.hpp"
#include "service_state.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace inputnext::xinput {
namespace {
[[nodiscard]] DWORD xinput_output_error(const output::error value) noexcept {
    switch (value) {
    case output::error::invalid_argument:
        return ERROR_BAD_ARGUMENTS;
    case output::error::not_connected:
    case output::error::stale_context:
    case output::error::revoked:
    case output::error::stale_revision:
    case output::error::expired:
        return ERROR_DEVICE_NOT_CONNECTED;
    case output::error::resources:
    case output::error::capacity_exceeded:
        return ERROR_NOT_ENOUGH_MEMORY;
    case output::error::identity_exhausted:
    case output::error::sequence_rejected:
    case output::error::revision_exhausted:
    case output::error::logical_version_exhausted:
    case output::error::deadline_overflow:
        return ERROR_GEN_FAILURE;
    }
    return ERROR_GEN_FAILURE;
}

[[nodiscard]] output::detail::xinput_rumble_key make_rumble_key(
    output::detail::xinput_service_id service,
    const assignment& occupant,
    std::uint64_t enable_generation,
    focus::foreground_epoch epoch) noexcept {
    return output::detail::xinput_rumble_key{.service = service,
        .occupant = occupant,
        .enable_generation = enable_generation,
        .epoch = epoch};
}
} // namespace

void service::implementation::clear_rumble_state(position_record& position,
    output::detail::xinput_rumble::transaction* turn,
    const DWORD error) noexcept {
    if (turn != nullptr) {
        turn->revoke_and_ack(position.rumble_context);
    }
    position.desired_rumble.reset();
    position.rumble_restore_pending = false;
    position.rumble_restore_error = error;
    position.rumble_logical_version = 0U;
}

void service::implementation::restore_rumble_state(position_record& position,
    output::detail::xinput_rumble::transaction& turn,
    const std::uint64_t enable_generation) const noexcept {
    if (!position.desired_rumble.has_value()) {
        return;
    }
    auto& desired = *position.desired_rumble;
    desired.enable_generation = enable_generation;
    const auto key =
        make_rumble_key(rumble->service_id(), desired.occupant, enable_generation, desired.epoch);
    const auto restored =
        turn.submit(key, position.rumble_context, desired.value, desired.request_revision);
    if (restored.has_value()) {
        position.rumble_logical_version = *restored;
        position.rumble_restore_pending = false;
        position.rumble_restore_error = ERROR_SUCCESS;
        return;
    }

    if (!turn.target_is_qualified(key) && !turn.qualify(key).has_value()) {
        clear_rumble_state(position, &turn, xinput_output_error(restored.error()));
        return;
    }
    position.rumble_restore_pending = true;
    position.rumble_restore_error = xinput_output_error(restored.error());
}

void service::implementation::transition_rumble_for_enable(position_record& position,
    output::detail::xinput_rumble::transaction* turn,
    const bool next_enabled,
    const enable_generation_transition generations) const noexcept {
    bool cache_matches{};
    position_record::desired_rumble_state preserved_cache{};
    if (turn != nullptr && position.occupant.has_value() && position.desired_rumble.has_value()) {
        const auto& desired = *position.desired_rumble;
        if (desired.occupant == *position.occupant &&
            desired.enable_generation == generations.previous) {
            cache_matches = true;
            preserved_cache = desired;
        }
    }
    const auto prior_restore_error = position.rumble_restore_error;
    bool cache_qualified{};
    if (!next_enabled && cache_matches) {
        const auto old_key = make_rumble_key(rumble->service_id(),
            preserved_cache.occupant,
            generations.previous,
            preserved_cache.epoch);
        cache_qualified = turn->qualify(old_key).has_value() && turn->target_is_qualified(old_key);
    }

    clear_rumble_state(position,
        turn,
        next_enabled ? prior_restore_error : static_cast<DWORD>(ERROR_SUCCESS));
    if (next_enabled && cache_matches) {
        position.desired_rumble = preserved_cache;
        restore_rumble_state(position, *turn, generations.next);
        return;
    }
    if (!next_enabled && cache_qualified) {
        preserved_cache.enable_generation = generations.next;
        position.desired_rumble = preserved_cache;
        return;
    }
}

void service::enable(BOOL enabled) {
    bool changed{};
    {
        std::optional<output::detail::xinput_rumble::transaction> output_turn;
        std::optional<focus::observer::presentation_guard> presentation_turn;
        if (implementation_->rumble != nullptr) {
            output_turn.emplace(implementation_->rumble->begin());
        } else {
            presentation_turn.emplace(implementation_->focus.lock_presentation());
        }
        auto* const turn = output_turn.has_value() ? &*output_turn : nullptr;
        [[maybe_unused]] auto const& guard =
            output_turn.has_value() ? output_turn->presentation() : *presentation_turn;
        if (!implementation_->closed) {
            const bool next_enabled = enabled != FALSE;
            if (next_enabled != implementation_->enabled) {
                changed = true;
                const auto previous_generation = implementation_->event_enable_generation;
                if (implementation_->event_enable_generation ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    implementation_->enabled = next_enabled;
                    implementation_->event_identity_exhausted = true;
                    for (auto& position : implementation_->positions) {
                        implementation::clear_rumble_state(position, turn, ERROR_SUCCESS);
                        static_cast<void>(implementation::invalidate_event_position(position,
                            implementation::guide_wait_state::identity_exhausted));
                    }
                } else {
                    ++implementation_->event_enable_generation;
                    implementation_->enabled = next_enabled;
                    for (auto& position : implementation_->positions) {
                        const auto next_generation = implementation_->event_enable_generation;
                        implementation_->transition_rumble_for_enable(position,
                            turn,
                            next_enabled,
                            implementation::enable_generation_transition{.previous =
                                                                             previous_generation,
                                .next = next_generation});
                        static_cast<void>(implementation::invalidate_event_position(position,
                            implementation::guide_wait_state::ineligible));
                    }
                }
            }
        }
    }
    if (changed) {
        implementation_->notify_guide_waiters();
        for (std::uint32_t index = 0U; index < 4U; ++index) {
            implementation_->cleanup_idle_event_reader(index);
        }
    }
}

DWORD service::implementation::prepare_rumble_request(DWORD user_index,
    const XINPUT_VIBRATION* vibration,
    prepared_rumble_request& prepared) {
    prepared = prepared_rumble_request{};
    if (user_index >= positions.size() || vibration == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }

    std::optional<read_pin> capture_pin;
    {
        const auto guard = focus.lock_presentation();
        if (closed || event_identity_exhausted) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        auto& position = positions[user_index];
        if (!position.occupant.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        prepared.occupant = *position.occupant;
        if (!live_binding_matches(position, prepared.occupant, capture_pin)) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        if (rumble == nullptr) {
            return ERROR_NOT_SUPPORTED;
        }

        const auto observed = guard.published();
        if (observed.open && !observed.exhausted && observed.state == focus::evidence::background) {
            prepared.known_background = true;
        } else if (!guard.matches_foreground(observed)) {
            return ERROR_DEVICE_NOT_CONNECTED;
        } else {
            const auto epoch = guard.current_foreground_epoch();
            if (!epoch.has_value()) {
                return ERROR_DEVICE_NOT_CONNECTED;
            }
            prepared.epoch = *epoch;
        }
        prepared.enable_generation = event_enable_generation;
        prepared.enabled = enabled;
    }

    if (prepared.known_background) {
        return ERROR_SUCCESS;
    }
    prepared.value = output::rumble_value{
        .low_frequency = static_cast<float>(vibration->wLeftMotorSpeed) / 65535.0F,
        .high_frequency = static_cast<float>(vibration->wRightMotorSpeed) / 65535.0F};

    return ERROR_SUCCESS;
}

DWORD service::implementation::commit_rumble_request(DWORD user_index,
    const prepared_rumble_request& prepared) {
    if (user_index >= positions.size()) {
        return ERROR_BAD_ARGUMENTS;
    }
    if (rumble == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    if (prepared.known_background) {
        return ERROR_SUCCESS;
    }

    auto turn = rumble->begin();
    auto& position = positions[user_index];
    if (closed || event_identity_exhausted ||
        !implementation::same_assignment(position.occupant, prepared.occupant) ||
        event_enable_generation != prepared.enable_generation || enabled != prepared.enabled) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (position.rumble_request_revision == std::numeric_limits<std::uint64_t>::max()) {
        return ERROR_GEN_FAILURE;
    }

    const auto next_revision = position.rumble_request_revision + 1U;
    const auto key = make_rumble_key(rumble->service_id(),
        prepared.occupant,
        prepared.enable_generation,
        prepared.epoch);
    if (!prepared.enabled) {
        if (position.rumble_context.has_value()) {
            turn.revoke_and_ack(position.rumble_context);
        }
        const auto qualified = turn.qualify(key);
        if (!qualified.has_value()) {
            return xinput_output_error(qualified.error());
        }
        if (!turn.target_is_qualified(key)) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        position.desired_rumble =
            implementation::position_record::desired_rumble_state{.value = prepared.value,
                .occupant = prepared.occupant,
                .epoch = prepared.epoch,
                .enable_generation = prepared.enable_generation,
                .request_revision = next_revision};
        position.rumble_request_revision = next_revision;
        position.rumble_logical_version = 0U;
        position.rumble_restore_pending = false;
        position.rumble_restore_error = ERROR_SUCCESS;
        return ERROR_SUCCESS;
    }

    if (position.rumble_context.has_value() && !turn.owns_key(key, position.rumble_context)) {
        turn.revoke_and_ack(position.rumble_context);
    }
    const auto submitted = turn.submit(key, position.rumble_context, prepared.value, next_revision);
    if (!submitted.has_value()) {
        return xinput_output_error(submitted.error());
    }

    position.desired_rumble =
        implementation::position_record::desired_rumble_state{.value = prepared.value,
            .occupant = prepared.occupant,
            .epoch = prepared.epoch,
            .enable_generation = prepared.enable_generation,
            .request_revision = next_revision};
    position.rumble_request_revision = next_revision;
    position.rumble_logical_version = *submitted;
    position.rumble_restore_pending = false;
    position.rumble_restore_error = ERROR_SUCCESS;
    return ERROR_SUCCESS;
}

DWORD service::set_state(flavor /*version*/, DWORD user_index, const XINPUT_VIBRATION* vibration) {
    implementation::prepared_rumble_request prepared{};
    const auto status = implementation_->prepare_rumble_request(user_index, vibration, prepared);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    if (prepared.known_background) {
        if (implementation_->rumble != nullptr) {
            maintain_output();
        }
        return ERROR_SUCCESS;
    }
    return implementation_->commit_rumble_request(user_index, prepared);
}

DWORD service::power_off_controller(flavor /*version*/, DWORD user_index) {
    if (user_index >= implementation_->positions.size()) {
        return ERROR_BAD_ARGUMENTS;
    }
    std::optional<read_pin> pin;
    const auto guard = implementation_->focus.lock_presentation();
    if (implementation_->closed) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    auto& position = implementation_->positions[user_index];
    if (!position.occupant.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!implementation::live_binding_matches(position, *position.occupant, pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    return ERROR_NOT_SUPPORTED;
}

void service::implementation::maintain_enabled_rumble(position_record& position,
    output::detail::xinput_rumble::transaction& turn,
    const output::detail::xinput_rumble_key& key) noexcept {
    const bool context_matches =
        position.rumble_context.has_value() && turn.owns_key(key, position.rumble_context);
    if (position.rumble_context.has_value() && !context_matches) {
        turn.revoke_and_ack(position.rumble_context);
        position.rumble_logical_version = 0U;
    }

    if (!position.rumble_restore_pending && context_matches) {
        const auto qualified = turn.qualify(key);
        if (!qualified.has_value()) {
            clear_rumble_state(position, &turn, xinput_output_error(qualified.error()));
        }
        return;
    }

    if (!position.desired_rumble.has_value()) {
        clear_rumble_state(position, &turn, ERROR_SUCCESS);
        return;
    }
    const auto& desired = *position.desired_rumble;
    position.rumble_restore_pending = true;
    const auto submitted =
        turn.submit(key, position.rumble_context, desired.value, desired.request_revision);
    if (submitted.has_value()) {
        position.rumble_logical_version = *submitted;
        position.rumble_restore_pending = false;
        position.rumble_restore_error = ERROR_SUCCESS;
        return;
    }

    const auto submit_error = xinput_output_error(submitted.error());
    if (!turn.target_is_qualified(key)) {
        const auto qualified = turn.qualify(key);
        if (!qualified.has_value()) {
            clear_rumble_state(position, &turn, xinput_output_error(qualified.error()));
            return;
        }
    }
    position.rumble_restore_pending = true;
    position.rumble_restore_error = submit_error;
}

void service::implementation::maintain_rumble_position(position_record& position,
    output::detail::xinput_rumble::transaction& turn) const noexcept {
    if (!position.desired_rumble.has_value() || !position.occupant.has_value()) {
        clear_rumble_state(position, &turn, ERROR_SUCCESS);
        return;
    }

    auto const& desired = *position.desired_rumble;
    if (closed || event_identity_exhausted || desired.occupant != *position.occupant) {
        clear_rumble_state(position, &turn, ERROR_SUCCESS);
        return;
    }
    if (desired.enable_generation != event_enable_generation ||
        !turn.presentation().matches_foreground_epoch(desired.epoch)) {
        clear_rumble_state(position, &turn, ERROR_SUCCESS);
        return;
    }

    const auto key = make_rumble_key(rumble->service_id(),
        desired.occupant,
        desired.enable_generation,
        desired.epoch);
    if (enabled) {
        maintain_enabled_rumble(position, turn, key);
        return;
    }

    const auto qualified = turn.qualify(key);
    if (!qualified.has_value()) {
        clear_rumble_state(position, &turn, xinput_output_error(qualified.error()));
    }
}

void service::maintain_output() noexcept {
    if (implementation_->rumble == nullptr) {
        return;
    }

    auto turn = implementation_->rumble->begin();
    for (auto& position : implementation_->positions) {
        implementation_->maintain_rumble_position(position, turn);
    }
}

std::optional<rumble_output_status> service::get_rumble_output_status(DWORD user_index) const {
    if (user_index >= implementation_->positions.size()) {
        return std::nullopt;
    }
    const auto guard = implementation_->focus.lock_presentation();
    const auto& position = implementation_->positions[user_index];
    return rumble_output_status{.configured = implementation_->rumble != nullptr,
        .desired_state_cached = position.desired_rumble.has_value(),
        .restore_pending = position.rumble_restore_pending,
        .last_restore_error = position.rumble_restore_error,
        .request_revision = position.rumble_request_revision,
        .logical_version = position.rumble_logical_version};
}

} // namespace inputnext::xinput
