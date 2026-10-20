#include <inputnext/xinput.hpp>

#include "service_state.hpp"
#include "translation.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/xinput_types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace inputnext::xinput {
namespace {

constexpr WORD guide_button_mask = 0x0400U;

[[nodiscard]] bool same_assignment(const std::optional<assignment>& current,
    const assignment& expected) noexcept {
    return current.has_value() && *current == expected;
}

} // namespace

service::implementation::implementation(focus::observer& focus_observer,
    metadata_source& metadata_provider,
    presentation_seed seed)
    : focus(focus_observer),
      metadata(metadata_provider),
      next_assignment(seed.next_assignment) {
    if (next_assignment == 0U) {
        throw std::invalid_argument{"XInput assignment seed must be nonzero"};
    }
}

service::service(focus::observer& focus, metadata_source& metadata, presentation_seed seed)
    : implementation_(std::make_unique<implementation>(focus, metadata, seed)) {
}

service::~service() = default;

DWORD service::implementation::prepare_state_read(DWORD user_index,
    bool include_guide,
    prepared_state_read& prepared) {
    prepared.include_guide = include_guide;
    {
        const auto guard = focus.lock_presentation();
        if (closed) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        const auto& position = positions[user_index];
        if (!position.occupant.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        prepared.occupant = *position.occupant;
        prepared.reader = position.reader;
        prepared.focus_word = guard.published();
        prepared.eligible_at_capture = enabled && guard.matches_foreground(prepared.focus_word);
    }

    if (!prepared.eligible_at_capture) {
        return ERROR_SUCCESS;
    }

    auto captured = prepared.reader.capture();
    if (!captured.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    prepared.sample_pin.emplace(std::move(*captured));
    if (prepared.sample_pin->target() != prepared.occupant.target) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const auto sample = prepared.sample_pin->read();
    if (!sample.has_value() || sample->target != prepared.occupant.target) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    prepared.sample_revision = sample->sample.revision;
    detail::translate_state(sample->sample.state, prepared.canonical_gamepad, true);
    return ERROR_SUCCESS;
}

DWORD service::implementation::commit_state_read(const prepared_state_read& prepared,
    XINPUT_STATE* output) {
    std::optional<read_pin> final_pin;
    {
        const auto guard = focus.lock_presentation();
        if (closed || prepared.occupant.user_index >= positions.size()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        auto& position = positions[prepared.occupant.user_index];
        if (!same_assignment(position.occupant, prepared.occupant)) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }

        auto final_capture = prepared.reader.capture();
        if (!final_capture.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        final_pin.emplace(std::move(*final_capture));
        if (final_pin->target() != prepared.occupant.target) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }

        const bool active = prepared.eligible_at_capture && enabled &&
                            guard.matches_foreground(prepared.focus_word);
        XINPUT_GAMEPAD selected_gamepad{};
        if (active) {
            if (!position.packet_initialized) {
                position.packet_initialized = true;
                position.packet_number = 1U;
                position.canonical_gamepad = prepared.canonical_gamepad;
                position.committed_revision = prepared.sample_revision;
            } else if (prepared.sample_revision > position.committed_revision) {
                position.committed_revision = prepared.sample_revision;
                if (!detail::same_gamepad(position.canonical_gamepad, prepared.canonical_gamepad)) {
                    ++position.packet_number;
                    position.canonical_gamepad = prepared.canonical_gamepad;
                }
            }
            selected_gamepad = position.canonical_gamepad;
            if (!prepared.include_guide) {
                selected_gamepad.wButtons = static_cast<WORD>(
                    selected_gamepad.wButtons & static_cast<WORD>(~guide_button_mask));
            }
        }

        XINPUT_STATE result_state{};
        result_state.dwPacketNumber = position.packet_number;
        if (active) {
            result_state.Gamepad = selected_gamepad;
        }
        *output = result_state;
    }
    return ERROR_SUCCESS;
}

result<assignment> service::assign(connection_target target, read_binding reader) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    std::optional<read_pin> candidate_pin;
    const auto guard = implementation_->focus.lock_presentation();
    if (implementation_->closed) {
        return std::unexpected{error::closed};
    }

    auto captured = reader.capture();
    if (!captured.has_value()) {
        return std::unexpected{captured.error() == error::invalid_argument ? error::invalid_argument
                                                                           : error::not_connected};
    }
    candidate_pin.emplace(std::move(*captured));
    if (candidate_pin->target() != target) {
        return std::unexpected{error::invalid_argument};
    }

    const auto existing = std::ranges::find_if(implementation_->positions,
        [target](const auto& row) { return row.occupant.value_or(assignment{}).target == target; });
    if (existing != implementation_->positions.end()) {
        return existing->occupant.value_or(assignment{});
    }

    const auto free = std::ranges::find_if(implementation_->positions,
        [](const auto& row) { return !row.occupant.has_value(); });
    if (free == implementation_->positions.end()) {
        return std::unexpected{error::capacity_exceeded};
    }
    if (implementation_->assignment_exhausted) {
        return std::unexpected{error::identity_exhausted};
    }

    const auto user_index =
        static_cast<std::uint32_t>(std::distance(implementation_->positions.begin(), free));
    const assignment result_assignment{
        .user_index = user_index,
        .id = implementation_->next_assignment,
        .target = target,
    };

    free->occupant = result_assignment;
    free->reader = reader;
    free->packet_initialized = false;
    free->canonical_gamepad = {};
    free->packet_number = 0U;
    free->committed_revision = 0U;

    if (implementation_->next_assignment == std::numeric_limits<std::uint64_t>::max()) {
        implementation_->assignment_exhausted = true;
    } else {
        ++implementation_->next_assignment;
    }
    return result_assignment;
}

result<void> service::unassign(assignment expected) {
    if (expected.user_index >= implementation_->positions.size() || expected.id == 0U ||
        !expected.target.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    const auto guard = implementation_->focus.lock_presentation();
    if (implementation_->closed) {
        return std::unexpected{error::closed};
    }
    auto& position = implementation_->positions[expected.user_index];
    if (!same_assignment(position.occupant, expected)) {
        return std::unexpected{error::not_connected};
    }

    position.reader = {};
    position.occupant.reset();
    position.packet_initialized = false;
    position.canonical_gamepad = {};
    position.packet_number = 0U;
    position.committed_revision = 0U;
    return {};
}

std::array<std::optional<assignment>, 4U> service::positions() const {
    const auto guard = implementation_->focus.lock_presentation();
    std::array<std::optional<assignment>, 4U> result_positions{};
    for (std::size_t index = 0U; index < implementation_->positions.size(); ++index) {
        result_positions[index] = implementation_->positions[index].occupant;
    }
    return result_positions;
}

void service::close() {
    const auto guard = implementation_->focus.lock_presentation();
    if (implementation_->closed) {
        return;
    }
    implementation_->closed = true;
    for (auto& position : implementation_->positions) {
        position.reader = {};
        position.occupant.reset();
        position.packet_initialized = false;
        position.canonical_gamepad = {};
        position.packet_number = 0U;
        position.committed_revision = 0U;
    }
}

void service::enable(BOOL enabled) {
    const auto guard = implementation_->focus.lock_presentation();
    if (!implementation_->closed) {
        implementation_->enabled = enabled != FALSE;
    }
}

DWORD service::get_state(flavor /*version*/,
    DWORD user_index,
    XINPUT_STATE* state,
    bool include_guide) {
    if (user_index >= implementation_->positions.size() || state == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }

    implementation::prepared_state_read prepared{};
    const auto prepared_status =
        implementation_->prepare_state_read(user_index, include_guide, prepared);
    if (prepared_status != ERROR_SUCCESS) {
        return prepared_status;
    }
    return implementation_->commit_state_read(prepared, state);
}

} // namespace inputnext::xinput
