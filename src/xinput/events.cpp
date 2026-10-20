#include <inputnext/xinput.hpp>

#include "keystroke_tracker.hpp"
#include "service_state.hpp"
#include "translation.hpp"
#include "xinput/keystroke_mapping.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace inputnext::xinput {
namespace {

[[nodiscard]] bool valid_source(history_source_kind value) noexcept {
    switch (value) {
    case history_source_kind::native_history:
    case history_source_kind::callback_capture:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_reset(history_reset value) noexcept {
    switch (value) {
    case history_reset::none:
    case history_reset::initial_open:
    case history_reset::history_gap:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_loss(history_loss value) noexcept {
    switch (value) {
    case history_loss::none:
    case history_loss::known:
    case history_loss::unknown:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_origin(history_record_origin value) noexcept {
    switch (value) {
    case history_record_origin::baseline:
    case history_record_origin::native_pad:
    case history_record_origin::callback_pad:
    case history_record_origin::callback_guide:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_failure(history_error value) noexcept {
    switch (value) {
    case history_error::invalid_argument:
    case history_error::invalid_reader:
    case history_error::closed:
    case history_error::disconnected:
    case history_error::busy:
    case history_error::capacity_exceeded:
    case history_error::identity_exhausted:
    case history_error::sequence_exhausted:
    case history_error::no_data:
    case history_error::history_lost:
    case history_error::provider_failure:
    case history_error::clock_discontinuity:
        return true;
    }
    return false;
}

[[nodiscard]] bool preserves_progress(history_error failure) noexcept {
    return failure == history_error::busy || failure == history_error::no_data;
}

} // namespace

bool service::implementation::stage_batch(const history_batch& batch,
    connection_target target,
    staged_event_batch& staged) noexcept {
    staged = {};
    if (!valid_batch_header(batch, target) || !valid_batch_shape(batch)) {
        return false;
    }

    const bool baseline_batch = batch.reset != history_reset::none;
    std::uint64_t previous_sequence{};
    for (std::size_t index = 0U; index < batch.count; ++index) {
        if (!stage_record(batch.records[index],
                index,
                baseline_batch,
                previous_sequence,
                staged.records[index])) {
            return false;
        }
        previous_sequence = batch.records[index].sequence;
    }
    if (batch.count == 0U) {
        staged.valid = true;
        return true;
    }
    if (staged.records[0U].sequence != batch.first_sequence ||
        staged.records[batch.count - 1U].sequence != batch.last_sequence ||
        (batch.caught_up && batch.checkpoint_sequence != batch.last_sequence)) {
        return false;
    }

    staged.count = batch.count;
    staged.valid = true;
    return true;
}

bool service::implementation::valid_batch_header(const history_batch& batch,
    connection_target target) noexcept {
    if (batch.count > batch.records.size() || batch.domain.target != target ||
        !valid_source(batch.source) || !valid_reset(batch.reset) || !valid_loss(batch.loss) ||
        (batch.failure.has_value() && !valid_failure(batch.failure->code))) {
        return false;
    }
    if (batch.caught_up) {
        return batch.checkpoint_sequence != 0U && batch.checkpoint_qpc != 0U &&
               !batch.failure.has_value();
    }
    return batch.checkpoint_sequence == 0U && batch.checkpoint_qpc == 0U;
}

bool service::implementation::valid_batch_shape(const history_batch& batch) noexcept {
    if (batch.count == 0U) {
        return batch.first_sequence == 0U && batch.last_sequence == 0U &&
               batch.reset == history_reset::none;
    }
    if (batch.domain.stream == 0U || batch.first_sequence == 0U || batch.last_sequence == 0U) {
        return false;
    }
    if (batch.reset != history_reset::none) {
        return batch.count == 1U && batch.first_sequence == 1U && batch.last_sequence == 1U &&
               batch.records[0U].baseline &&
               batch.records[0U].origin == history_record_origin::baseline &&
               (batch.reset == history_reset::initial_open ||
                   batch.reset == history_reset::history_gap);
    }
    return batch.loss == history_loss::none && batch.lost_count == 0U;
}

bool service::implementation::stage_record(const history_record& record,
    std::size_t index,
    bool baseline_batch,
    std::uint64_t previous_sequence,
    staged_event_record& staged) noexcept {
    if (!is_valid_gamepad_state(record.sample.state) || record.sequence == 0U ||
        !valid_origin(record.origin)) {
        return false;
    }
    if (baseline_batch) {
        if (record.sample.effective_qpc == 0U) {
            return false;
        }
    } else if (record.baseline || record.origin == history_record_origin::baseline ||
               (index != 0U && (previous_sequence == std::numeric_limits<std::uint64_t>::max() ||
                                   record.sequence != previous_sequence + 1U))) {
        return false;
    }

    XINPUT_GAMEPAD gamepad{};
    detail::translate_state(record.sample.state, gamepad, true);
    staged = staged_event_record{
        .keys = detail::keystroke_sample{.mask = detail::map_keystroke_keys(gamepad),
            .qpc = record.sample.effective_qpc},
        .sequence = record.sequence,
        .baseline = record.baseline,
        .guide_known = record.sample.guide_known,
        .guide_pressed =
            (record.sample.state.buttons & static_cast<button_mask>(gamepad_button::guide)) != 0U};
    return true;
}

void service::implementation::clear_history_state(event_position_state& events) noexcept {
    events.domain.reset();
    events.last_sequence = 0U;
    events.last_consumer_qpc = 0U;
    events.accepted_checkpoint_sequence = 0U;
    events.accepted_checkpoint_qpc = 0U;
    events.guide_known = false;
    events.guide_pressed = false;
    events.tracker.clear();
}

void service::implementation::set_guide_terminal(event_position_state& events,
    guide_wait_state state) noexcept {
    if (events.guide_wait.has_value() &&
        (events.guide_wait->state == guide_wait_state::pending ||
            events.guide_wait->state == guide_wait_state::signalled)) {
        events.guide_wait->state = state;
    }
}

service::implementation::guide_wait_state service::implementation::history_failure_state(
    history_failure failure) noexcept {
    switch (failure.code) {
    case history_error::closed:
        return guide_wait_state::closed;
    case history_error::disconnected:
        return guide_wait_state::not_connected;
    case history_error::identity_exhausted:
        return guide_wait_state::identity_exhausted;
    default:
        return guide_wait_state::history_reset;
    }
}

bool service::implementation::same_qualification(const event_position_state& events,
    const assignment& occupant,
    const focus::foreground_epoch& epoch,
    std::uint64_t enable_generation) noexcept {
    return events.qualification.has_value() && events.qualification->occupant == occupant &&
           events.qualification->focus_epoch == epoch &&
           events.qualification->enable_generation == enable_generation;
}

bool service::implementation::batch_matches_history(const event_position_state& events,
    const history_batch& batch,
    const staged_event_batch& staged) noexcept {
    if (!staged.valid) {
        return false;
    }
    if (staged.count == 0U) {
        if (!events.domain.has_value()) {
            return true;
        }
        return batch.domain.stream == 0U || batch.domain == *events.domain;
    }
    if (batch.reset != history_reset::none) {
        return batch.reset != history_reset::initial_open || !events.domain.has_value();
    }
    if (!events.domain.has_value()) {
        return false;
    }
    if (batch.domain != *events.domain ||
        events.last_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    return batch.first_sequence == events.last_sequence + 1U;
}

bool service::implementation::apply_reset_batch(event_position_state& events,
    const assignment& occupant,
    const focus::foreground_epoch& epoch,
    std::uint64_t enable_generation,
    const history_batch& batch,
    const staged_event_batch& staged) noexcept {
    const bool initial_for_this_consumer = !events.domain.has_value();
    if (!initial_for_this_consumer) {
        set_guide_terminal(events, guide_wait_state::history_reset);
    }
    clear_history_state(events);
    const auto& baseline = staged.records[0U];
    if (events.tracker.silent_seed(baseline.keys) != detail::keystroke_operation_result::applied) {
        return false;
    }
    events.domain = batch.domain;
    events.last_sequence = baseline.sequence;
    events.last_consumer_qpc = baseline.keys.qpc;
    events.guide_known = baseline.guide_known;
    events.guide_pressed = baseline.guide_known && baseline.guide_pressed;

    auto& ticket = events.guide_wait;
    if (initial_for_this_consumer && ticket.has_value() &&
        ticket->state == guide_wait_state::pending && ticket->occupant == occupant &&
        ticket->focus_epoch == epoch && ticket->enable_generation == enable_generation) {
        ticket->domain = batch.domain;
    }
    return true;
}

bool service::implementation::apply_event_records(event_position_state& events,
    const assignment& occupant,
    const focus::foreground_epoch& epoch,
    std::uint64_t enable_generation,
    const history_batch& batch,
    const staged_event_batch& staged,
    detail::key_mask& per_turn_repeat_mask) const noexcept {
    const auto timing = repeat_timing;
    if (!timing.has_value()) {
        return false;
    }
    for (std::size_t index = 0U; index < staged.count; ++index) {
        const auto& record = staged.records[index];
        const auto event_qpc = record.keys.qpc == 0U
                                   ? events.last_consumer_qpc
                                   : std::max(record.keys.qpc, events.last_consumer_qpc);
        if (event_qpc == 0U ||
            events.tracker.checked_advance_repeats(event_qpc,
                false,
                *timing,
                per_turn_repeat_mask) != detail::keystroke_operation_result::applied ||
            events.tracker.checked_apply_transitions(
                detail::keystroke_sample{.mask = record.keys.mask, .qpc = event_qpc}) !=
                detail::keystroke_operation_result::applied) {
            return false;
        }

        const bool guide_edge = events.guide_known && record.guide_known && !events.guide_pressed &&
                                record.guide_pressed;
        if (record.guide_known) {
            if (!events.guide_known) {
                events.guide_known = true;
                events.guide_pressed = record.guide_pressed;
            } else {
                events.guide_pressed = record.guide_pressed;
            }
        } else {
            events.guide_known = false;
            events.guide_pressed = false;
        }
        events.last_sequence = record.sequence;
        events.last_consumer_qpc = event_qpc;

        auto& ticket = events.guide_wait;
        if (guide_edge && ticket.has_value() && ticket->state == guide_wait_state::pending &&
            ticket->domain.has_value() && ticket->occupant == occupant &&
            ticket->focus_epoch == epoch && ticket->enable_generation == enable_generation &&
            *ticket->domain == batch.domain) {
            ticket->state = guide_wait_state::signalled;
        }
    }
    return true;
}

bool service::implementation::apply_checkpoint(event_position_state& events,
    const history_batch& batch,
    detail::key_mask& per_turn_repeat_mask) const noexcept {
    const auto timing = repeat_timing;
    if (!timing.has_value() || !events.domain.has_value()) {
        return false;
    }
    if (batch.domain != *events.domain || batch.checkpoint_sequence != events.last_sequence ||
        batch.checkpoint_sequence < events.accepted_checkpoint_sequence ||
        batch.checkpoint_qpc < events.last_consumer_qpc ||
        batch.checkpoint_qpc < events.accepted_checkpoint_qpc ||
        events.tracker.checked_advance_repeats(batch.checkpoint_qpc,
            true,
            *timing,
            per_turn_repeat_mask) != detail::keystroke_operation_result::applied) {
        return false;
    }
    events.last_consumer_qpc = batch.checkpoint_qpc;
    events.accepted_checkpoint_sequence = batch.checkpoint_sequence;
    events.accepted_checkpoint_qpc = batch.checkpoint_qpc;
    return true;
}

bool service::implementation::apply_batch(event_position_state& events,
    const assignment& occupant,
    const focus::foreground_epoch& epoch,
    std::uint64_t enable_generation,
    const history_batch& batch,
    const staged_event_batch& staged) const noexcept {
    if (!batch_matches_history(events, batch, staged)) {
        return false;
    }
    detail::key_mask per_turn_repeat_mask{};
    bool applied{};
    if (batch.reset != history_reset::none) {
        applied = apply_reset_batch(events, occupant, epoch, enable_generation, batch, staged);
    } else {
        applied = apply_event_records(events,
            occupant,
            epoch,
            enable_generation,
            batch,
            staged,
            per_turn_repeat_mask);
    }
    if (!applied || (batch.caught_up && !apply_checkpoint(events, batch, per_turn_repeat_mask))) {
        return false;
    }
    if (!batch.failure.has_value()) {
        return true;
    }
    if (staged.count == 0U && preserves_progress(batch.failure->code)) {
        return true;
    }
    set_guide_terminal(events, history_failure_state(*batch.failure));
    clear_history_state(events);
    events.cleanup_pending = true;
    return true;
}

bool service::implementation::current_capture_is_qualified(
    focus::observer::presentation_guard& guard,
    const event_qualification& qualification,
    const focus::snapshot& captured) const noexcept {
    if (closed || event_identity_exhausted || !enabled ||
        qualification.enable_generation != event_enable_generation ||
        !guard.matches_foreground(captured) ||
        !guard.matches_foreground_epoch(qualification.focus_epoch)) {
        return false;
    }
    const auto current_epoch = guard.current_foreground_epoch();
    if (!current_epoch.has_value() || *current_epoch != qualification.focus_epoch ||
        qualification.occupant.user_index >= positions.size()) {
        return false;
    }
    const auto& position = positions[qualification.occupant.user_index];
    return position.occupant.has_value() && *position.occupant == qualification.occupant &&
           same_qualification(position.events,
               qualification.occupant,
               qualification.focus_epoch,
               qualification.enable_generation);
}

bool service::implementation::live_binding_matches(position_record& position,
    const assignment& occupant,
    std::optional<read_pin>& pin) noexcept {
    if (!position.occupant.has_value() || *position.occupant != occupant) {
        return false;
    }
    auto captured = position.reader.capture();
    if (!captured.has_value()) {
        return false;
    }
    pin.emplace(std::move(*captured));
    return pin->target() == occupant.target;
}

void service::implementation::finish_reserved_operation(std::uint32_t user_index,
    std::optional<history_reader_handle> handle,
    bool close_succeeded,
    bool close_attempted) noexcept {
    const auto guard = focus.lock_presentation();
    if (user_index >= 4U) {
        return;
    }
    auto& events = positions[user_index].events;
    if (close_attempted && handle.has_value() && events.reader == handle) {
        if (close_succeeded) {
            events.reader.reset();
            events.cleanup_pending = false;
        } else {
            events.cleanup_pending = true;
        }
    }
    events.operation_reserved = false;
}

void service::implementation::cleanup_idle_event_reader(std::uint32_t user_index) noexcept {
    if (event_source_provider == nullptr || user_index >= 4U) {
        return;
    }

    std::optional<history_reader_handle> reader;
    {
        const auto guard = focus.lock_presentation();
        auto& events = positions[user_index].events;
        if (!events.reader.has_value() || events.operation_reserved || !events.cleanup_pending) {
            return;
        }
        events.operation_reserved = true;
        events.cleanup_pending = true;
        reader = events.reader;
    }

    const auto close_result = event_source_provider->close_history(*reader);
    finish_reserved_operation(user_index, reader, close_result.has_value(), true);
}

bool service::implementation::capture_event_turn(focus::observer::presentation_guard& guard,
    position_record& position,
    event_turn& turn,
    bool& notify) noexcept {
    turn.occupant = position.occupant;
    if (turn.occupant.has_value()) {
        auto captured = position.reader.capture();
        if (captured.has_value()) {
            turn.lifetime_pin.emplace(std::move(*captured));
            turn.connected = turn.lifetime_pin->target() == turn.occupant->target;
        }
    }
    turn.captured_focus = guard.published();
    const auto current_epoch = guard.current_foreground_epoch();
    turn.eligible = turn.provider != nullptr && turn.occupant.has_value() && turn.connected &&
                    !closed && !event_identity_exhausted && enabled && current_epoch.has_value() &&
                    guard.matches_foreground(turn.captured_focus) &&
                    guard.matches_foreground_epoch(*current_epoch);
    if (!turn.eligible) {
        auto reason = guide_wait_state::ineligible;
        if (closed) {
            reason = guide_wait_state::closed;
        } else if (event_identity_exhausted) {
            reason = guide_wait_state::identity_exhausted;
        } else if (turn.occupant.has_value() && !turn.connected) {
            reason = guide_wait_state::not_connected;
        }
        notify = invalidate_event_position(position, reason) || notify;
        return false;
    }

    const auto occupant = turn.occupant.value_or(assignment{});
    const event_qualification wanted{.occupant = occupant,
        .focus_epoch = *current_epoch,
        .enable_generation = event_enable_generation};
    turn.qualification = wanted;
    auto& events = position.events;
    if (!events.qualification.has_value() || *events.qualification != wanted) {
        auto reason = guide_wait_state::ineligible;
        if (events.qualification.has_value() && events.qualification->occupant != occupant) {
            reason = guide_wait_state::not_connected;
        }
        notify = invalidate_event_position(position, reason) || notify;
        events.qualification = wanted;
    }
    return true;
}

void service::implementation::reserve_event_operation(position_record& position,
    event_turn& turn) noexcept {
    auto& events = position.events;
    if (turn.provider == nullptr || (!events.reader.has_value() && events.cleanup_pending)) {
        return;
    }
    if (events.operation_reserved) {
        return;
    }
    if (events.reader.has_value() && (events.cleanup_pending || !turn.eligible)) {
        events.operation_reserved = true;
        turn.operation_reserved = true;
        turn.cleanup_only = true;
        turn.reader = events.reader;
        return;
    }
    if (!turn.eligible) {
        return;
    }
    events.operation_reserved = true;
    turn.operation_reserved = true;
    if (events.reader.has_value()) {
        turn.reader = events.reader;
    } else {
        turn.needs_open = true;
    }
}

bool service::implementation::start_event_turn(event_turn& turn) noexcept {
    turn.provider = event_source_provider;
    bool notify{};
    {
        auto guard = focus.lock_presentation();
        auto& position = positions[turn.user_index];
        static_cast<void>(capture_event_turn(guard, position, turn, notify));
        reserve_event_operation(position, turn);
    }
    if (notify) {
        notify_guide_waiters();
    }
    return turn.operation_reserved;
}

void service::implementation::close_event_reader(event_turn& turn) noexcept {
    if (turn.provider == nullptr || !turn.reader.has_value()) {
        finish_reserved_operation(turn.user_index, turn.reader, false, false);
        return;
    }
    const auto exact_reader = *turn.reader;
    const auto closed_reader = turn.provider->close_history(exact_reader);
    finish_reserved_operation(turn.user_index, turn.reader, closed_reader.has_value(), true);
}

void service::implementation::handle_event_open_failure(event_turn& turn,
    history_failure failure) noexcept {
    if (!turn.occupant.has_value()) {
        finish_reserved_operation(turn.user_index, std::nullopt, false, false);
        return;
    }
    const auto occupant = turn.occupant.value_or(assignment{});
    bool notify{};
    {
        std::optional<read_pin> final_pin;
        auto guard = focus.lock_presentation();
        auto& position = positions[turn.user_index];
        const bool same_assignment =
            position.occupant.has_value() && *position.occupant == occupant;
        const bool live = same_assignment && live_binding_matches(position, occupant, final_pin);
        const bool owns_qualification = same_qualification(position.events,
            turn.qualification.occupant,
            turn.qualification.focus_epoch,
            turn.qualification.enable_generation);
        const bool current =
            live && owns_qualification &&
            current_capture_is_qualified(guard, turn.qualification, turn.captured_focus);
        if (same_assignment && owns_qualification && !current) {
            const auto reason =
                live ? guide_wait_state::ineligible : guide_wait_state::not_connected;
            notify = invalidate_event_position(position, reason) || notify;
        }
        if (current && !preserves_progress(failure.code)) {
            set_guide_terminal(position.events, history_failure_state(failure));
            clear_history_state(position.events);
            notify = true;
        }
    }
    if (notify) {
        notify_guide_waiters();
    }
    finish_reserved_operation(turn.user_index, std::nullopt, false, false);
}

bool service::implementation::install_event_reader(event_turn& turn,
    history_reader_handle reader) noexcept {
    if (!turn.occupant.has_value()) {
        turn.reader = reader;
        close_event_reader(turn);
        return false;
    }
    const auto occupant = turn.occupant.value_or(assignment{});
    turn.reader = reader;
    bool may_read{};
    bool notify{};
    {
        std::optional<read_pin> final_pin;
        auto guard = focus.lock_presentation();
        auto& position = positions[turn.user_index];
        const bool same_assignment =
            position.occupant.has_value() && *position.occupant == occupant;
        const bool live = same_assignment && live_binding_matches(position, occupant, final_pin);
        const bool owns_qualification = same_qualification(position.events,
            turn.qualification.occupant,
            turn.qualification.focus_epoch,
            turn.qualification.enable_generation);
        const bool current =
            live && owns_qualification &&
            current_capture_is_qualified(guard, turn.qualification, turn.captured_focus);
        auto& events = position.events;
        events.reader = reader;
        if (current && events.operation_reserved && reader.target == occupant.target &&
            reader.value != 0U) {
            events.cleanup_pending = false;
            may_read = true;
        } else {
            events.cleanup_pending = true;
            if (same_assignment && owns_qualification && !current) {
                auto reason = guide_wait_state::ineligible;
                if (!live) {
                    reason = guide_wait_state::not_connected;
                }
                notify = invalidate_event_position(position, reason) || notify;
                events.reader = reader;
                events.cleanup_pending = true;
            }
        }
    }
    if (notify) {
        notify_guide_waiters();
    }
    if (!may_read) {
        close_event_reader(turn);
    }
    return may_read;
}

bool service::implementation::open_event_reader(event_turn& turn, history_time now) noexcept {
    if (turn.provider == nullptr || !turn.occupant.has_value()) {
        finish_reserved_operation(turn.user_index, std::nullopt, false, false);
        return false;
    }
    const auto occupant = turn.occupant.value_or(assignment{});
    const auto opened = turn.provider->open_history(turn.user_index, occupant.target, now);
    if (!opened.has_value()) {
        handle_event_open_failure(turn, opened.error());
        return false;
    }
    return install_event_reader(turn, *opened);
}

bool service::implementation::reject_stale_event_batch(position_record& position,
    bool same_assignment,
    bool owns_qualification,
    bool current,
    bool live,
    bool owns_reader,
    bool& notify) noexcept {
    if (same_assignment && owns_qualification && !current) {
        const auto reason = live ? guide_wait_state::ineligible : guide_wait_state::not_connected;
        notify = invalidate_event_position(position, reason) || notify;
    }
    if (!owns_reader) {
        return false;
    }
    position.events.cleanup_pending = true;
    return true;
}

bool service::implementation::apply_current_event_batch(event_position_state& events,
    const assignment& occupant,
    const event_turn& turn,
    const history_batch& batch,
    const staged_event_batch& staged,
    bool staged_valid,
    bool& notify) const noexcept {
    if (!staged_valid || !apply_batch(events,
                             occupant,
                             turn.qualification.focus_epoch,
                             turn.qualification.enable_generation,
                             batch,
                             staged)) {
        set_guide_terminal(events, guide_wait_state::history_reset);
        clear_history_state(events);
        events.cleanup_pending = true;
        notify = true;
        return true;
    }
    if (batch.failure.has_value() &&
        (staged.count != 0U || !preserves_progress(batch.failure->code))) {
        events.cleanup_pending = true;
        return true;
    }
    if (events.guide_wait.has_value() && events.guide_wait->state != guide_wait_state::pending) {
        notify = true;
    }
    return false;
}

void service::implementation::commit_event_batch(event_turn& turn,
    const history_batch& batch) noexcept {
    if (!turn.occupant.has_value() || !turn.reader.has_value()) {
        finish_reserved_operation(turn.user_index, turn.reader, false, false);
        return;
    }
    const auto occupant = turn.occupant.value_or(assignment{});
    staged_event_batch staged{};
    const bool staged_valid = stage_batch(batch, occupant.target, staged);
    bool must_close{};
    bool notify{};
    {
        std::optional<read_pin> final_pin;
        auto guard = focus.lock_presentation();
        auto& position = positions[turn.user_index];
        auto& events = position.events;
        const bool same_assignment =
            position.occupant.has_value() && *position.occupant == occupant;
        const bool live = same_assignment && live_binding_matches(position, occupant, final_pin);
        const bool owns_qualification = same_qualification(events,
            turn.qualification.occupant,
            turn.qualification.focus_epoch,
            turn.qualification.enable_generation);
        const bool current =
            live && owns_qualification &&
            current_capture_is_qualified(guard, turn.qualification, turn.captured_focus);
        const bool owns_reader = events.reader == turn.reader;
        if (!same_assignment || !current || !owns_reader || events.cleanup_pending) {
            must_close = reject_stale_event_batch(position,
                same_assignment,
                owns_qualification,
                current,
                live,
                owns_reader,
                notify);
        } else {
            must_close = apply_current_event_batch(events,
                occupant,
                turn,
                batch,
                staged,
                staged_valid,
                notify);
        }
    }
    if (notify) {
        notify_guide_waiters();
    }
    if (must_close) {
        close_event_reader(turn);
    } else {
        finish_reserved_operation(turn.user_index, turn.reader, false, false);
    }
}

void service::implementation::pump_event_turn(std::uint32_t user_index, history_time now) noexcept {
    if (user_index >= 4U) {
        return;
    }
    event_turn turn{.user_index = user_index};
    if (!start_event_turn(turn)) {
        return;
    }
    if (turn.cleanup_only) {
        close_event_reader(turn);
        return;
    }
    if (!turn.occupant.has_value() || !turn.lifetime_pin.has_value()) {
        finish_reserved_operation(user_index, turn.reader, false, false);
        return;
    }
    if (turn.needs_open && !open_event_reader(turn, now)) {
        return;
    }
    if (turn.provider == nullptr || !turn.reader.has_value()) {
        finish_reserved_operation(user_index, turn.reader, false, false);
        return;
    }
    const auto batch = turn.provider->read_history(*turn.reader, now);
    commit_event_batch(turn, batch);
}

void service::pump_events(DWORD user_index, history_time now) {
    if (user_index >= 4U) {
        return;
    }
    implementation_->pump_event_turn(user_index, now);
}

} // namespace inputnext::xinput
