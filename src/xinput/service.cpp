#include <inputnext/xinput.hpp>

#include "keystroke_tracker.hpp"
#include "service_state.hpp"
#include "translation.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput_events.hpp>
#include <inputnext/xinput_types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace inputnext::xinput {
namespace {

constexpr WORD guide_button_mask = 0x0400U;

} // namespace

service::implementation::implementation(focus::observer& focus_observer,
    metadata_source& metadata_provider,
    event_source* event_provider,
    std::uint64_t qpc_frequency,
    std::unique_ptr<output::detail::xinput_rumble> rumble_bridge,
    presentation_seed seed)
    : focus(focus_observer),
      metadata(metadata_provider),
      event_source_provider(event_provider),
      rumble(std::move(rumble_bridge)),
      next_assignment(seed.next_assignment) {
    if (rumble != nullptr && !rumble->matches_focus(focus)) {
        throw std::invalid_argument{
            "configured XInput output must share its authority focus observer"};
    }
    if (next_assignment == 0U) {
        throw std::invalid_argument{"XInput assignment seed must be nonzero"};
    }
    if (event_source_provider != nullptr) {
        const auto timing = detail::keystroke_repeat_timing::from_qpc_frequency(qpc_frequency);
        if (!timing.has_value()) {
            throw std::invalid_argument{"XInput event service requires a valid QPC frequency"};
        }
        repeat_timing.emplace(*timing);
    }
}

void service::implementation::clear_event_stream(event_position_state& events) noexcept {
    events.qualification.reset();
    clear_history_state(events);
    if (events.reader.has_value()) {
        events.cleanup_pending = true;
    }
}

bool service::implementation::invalidate_event_position(position_record& position,
    guide_wait_state reason) noexcept {
    auto& events = position.events;
    auto changed = events.qualification.has_value() || events.domain.has_value();
    clear_event_stream(events);
    if (events.guide_wait.has_value() &&
        (events.guide_wait->state == guide_wait_state::pending ||
            events.guide_wait->state == guide_wait_state::signalled)) {
        events.guide_wait->state = reason;
        changed = true;
    }
    return changed;
}

void service::implementation::notify_guide_waiters() noexcept {
    std::scoped_lock wait_lock{guide_wait_mutex};
    guide_wait_changed.notify_all();
}

bool service::implementation::event_qualification_is_current(const event_position_state& events,
    const assignment& occupant,
    const focus::foreground_epoch& epoch) const noexcept {
    return !event_identity_exhausted && enabled && events.qualification.has_value() &&
           events.qualification->occupant == occupant &&
           events.qualification->focus_epoch == epoch &&
           events.qualification->enable_generation == event_enable_generation;
}

service::service(focus::observer& focus, metadata_source& metadata, presentation_seed seed)
    : implementation_(
          std::make_unique<implementation>(focus, metadata, nullptr, 0U, nullptr, seed)) {
}

service::service(focus::observer& focus,
    metadata_source& metadata,
    event_source& events,
    std::uint64_t qpc_frequency,
    presentation_seed seed)
    : implementation_(std::make_unique<implementation>(focus,
          metadata,
          &events,
          qpc_frequency,
          nullptr,
          seed)) {
}

service::service(focus::observer& focus,
    metadata_source& metadata,
    std::unique_ptr<output::detail::xinput_rumble> rumble,
    presentation_seed seed)
    : implementation_(
          std::make_unique<implementation>(focus, metadata, nullptr, 0U, std::move(rumble), seed)) {
}

service::service(focus::observer& focus,
    metadata_source& metadata,
    event_source& events,
    std::uint64_t qpc_frequency,
    std::unique_ptr<output::detail::xinput_rumble> rumble,
    presentation_seed seed)
    : implementation_(std::make_unique<implementation>(focus,
          metadata,
          &events,
          qpc_frequency,
          std::move(rumble),
          seed)) {
}

service::~service() {
    close();
}

DWORD service::implementation::prepare_state_read(DWORD user_index,
    bool include_guide,
    prepared_state_read& prepared) {
    prepared.include_guide = include_guide;
    read_binding reader{};
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
        reader = position.reader;
        const auto captured_foreground = guard.capture_foreground();
        if (captured_foreground.has_value()) {
            prepared.focus_word = *captured_foreground;
        }
        prepared.eligible_at_capture = captured_foreground.has_value() && enabled;
    }

    auto captured = reader.capture();
    if (!captured.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    prepared.connection_pin = std::move(*captured);
    if (prepared.connection_pin.target() != prepared.occupant.target) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!prepared.eligible_at_capture) {
        return ERROR_SUCCESS;
    }
    const auto sample = prepared.connection_pin.read();
    if (!sample.has_value() || sample->target != prepared.occupant.target) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    prepared.sample_revision = sample->sample.revision;
    detail::translate_state(sample->sample.state, prepared.canonical_gamepad, true);
    return ERROR_SUCCESS;
}

DWORD service::implementation::commit_state_read(const prepared_state_read& prepared,
    XINPUT_STATE* output) {
    {
        const auto guard = focus.lock_presentation();
        if (closed || prepared.occupant.user_index >= positions.size()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        auto& position = positions[prepared.occupant.user_index];
        if (!implementation::same_assignment(position.occupant, prepared.occupant)) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }

        if (prepared.connection_pin.target() != prepared.occupant.target ||
            !prepared.connection_pin.validate_live().has_value()) {
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
    std::optional<assignment> result_assignment;
    {
        std::optional<output::detail::xinput_rumble::transaction> output_turn;
        std::optional<focus::observer::presentation_guard> presentation_turn;
        if (implementation_->rumble != nullptr) {
            output_turn.emplace(implementation_->rumble->begin());
        } else {
            presentation_turn.emplace(implementation_->focus.lock_presentation());
        }
        [[maybe_unused]] auto const& guard =
            output_turn.has_value() ? output_turn->presentation() : *presentation_turn;
        if (implementation_->closed) {
            return std::unexpected{error::closed};
        }

        auto captured = reader.capture();
        if (!captured.has_value()) {
            return std::unexpected{captured.error() == error::invalid_argument
                                       ? error::invalid_argument
                                       : error::not_connected};
        }
        candidate_pin.emplace(std::move(*captured));
        if (candidate_pin->target() != target) {
            return std::unexpected{error::invalid_argument};
        }

        const auto existing =
            std::ranges::find_if(implementation_->positions, [target](const auto& row) {
                return row.occupant.value_or(assignment{}).target == target;
            });
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
        result_assignment = assignment{.user_index = user_index,
            .id = implementation_->next_assignment,
            .target = target};

        if (output_turn.has_value()) {
            output_turn->revoke_and_ack(free->rumble_context);
        }
        free->desired_rumble.reset();
        free->rumble_restore_pending = false;
        free->rumble_restore_error = ERROR_SUCCESS;
        free->rumble_logical_version = 0U;
        free->occupant = *result_assignment;
        free->reader = reader;
        free->packet_initialized = false;
        free->canonical_gamepad = {};
        free->packet_number = 0U;
        free->committed_revision = 0U;
        implementation::clear_event_stream(free->events);

        if (implementation_->next_assignment == std::numeric_limits<std::uint64_t>::max()) {
            implementation_->assignment_exhausted = true;
        } else {
            ++implementation_->next_assignment;
        }
    }
    implementation_->notify_guide_waiters();
    implementation_->cleanup_idle_event_reader(result_assignment->user_index);
    return *result_assignment;
}

result<void> service::unassign(assignment expected) {
    if (expected.user_index >= implementation_->positions.size() || expected.id == 0U ||
        !expected.target.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    {
        std::optional<output::detail::xinput_rumble::transaction> output_turn;
        std::optional<focus::observer::presentation_guard> presentation_turn;
        if (implementation_->rumble != nullptr) {
            output_turn.emplace(implementation_->rumble->begin());
        } else {
            presentation_turn.emplace(implementation_->focus.lock_presentation());
        }
        [[maybe_unused]] auto const& guard =
            output_turn.has_value() ? output_turn->presentation() : *presentation_turn;
        if (implementation_->closed) {
            return std::unexpected{error::closed};
        }
        auto& position = implementation_->positions[expected.user_index];
        if (!implementation::same_assignment(position.occupant, expected)) {
            return std::unexpected{error::not_connected};
        }

        if (output_turn.has_value()) {
            output_turn->revoke_and_ack(position.rumble_context);
        }
        position.desired_rumble.reset();
        position.rumble_restore_pending = false;
        position.rumble_restore_error = ERROR_SUCCESS;
        position.rumble_logical_version = 0U;
        static_cast<void>(implementation::invalidate_event_position(position,
            implementation::guide_wait_state::not_connected));
        position.reader = {};
        position.occupant.reset();
        position.packet_initialized = false;
        position.canonical_gamepad = {};
        position.packet_number = 0U;
        position.committed_revision = 0U;
    }

    implementation_->notify_guide_waiters();
    implementation_->cleanup_idle_event_reader(expected.user_index);
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
    {
        std::optional<output::detail::xinput_rumble::transaction> output_turn;
        std::optional<focus::observer::presentation_guard> presentation_turn;
        if (implementation_->rumble != nullptr) {
            output_turn.emplace(implementation_->rumble->begin());
        } else {
            presentation_turn.emplace(implementation_->focus.lock_presentation());
        }
        [[maybe_unused]] auto const& guard =
            output_turn.has_value() ? output_turn->presentation() : *presentation_turn;
        implementation_->closed = true;
        for (auto& position : implementation_->positions) {
            if (output_turn.has_value()) {
                output_turn->revoke_and_ack(position.rumble_context);
            }
            position.desired_rumble.reset();
            position.rumble_restore_pending = false;
            position.rumble_restore_error = ERROR_SUCCESS;
            position.rumble_logical_version = 0U;
            static_cast<void>(implementation::invalidate_event_position(position,
                implementation::guide_wait_state::closed));
            position.reader = {};
            position.occupant.reset();
            position.packet_initialized = false;
            position.canonical_gamepad = {};
            position.packet_number = 0U;
            position.committed_revision = 0U;
        }
    }
    implementation_->notify_guide_waiters();
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        implementation_->cleanup_idle_event_reader(index);
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
