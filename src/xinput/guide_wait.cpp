#include <inputnext/xinput.hpp>

#include "service_state.hpp"
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput_events.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::xinput {
namespace {

constexpr auto qualification_poll_interval = std::chrono::milliseconds{4};

} // namespace

bool service::implementation::guide_ticket_is_fresh(focus::observer::presentation_guard& guard,
    position_record& position,
    const guide_wait_record& ticket,
    std::optional<read_pin>& pin,
    guide_wait_state& failure) const noexcept {
    if (closed) {
        failure = guide_wait_state::closed;
        return false;
    }
    if (!position.occupant.has_value() || *position.occupant != ticket.occupant) {
        failure = guide_wait_state::not_connected;
        return false;
    }
    auto captured = position.reader.capture();
    if (!captured.has_value() || captured->target() != ticket.occupant.target) {
        failure = guide_wait_state::not_connected;
        return false;
    }
    pin.emplace(std::move(*captured));
    if (event_identity_exhausted) {
        failure = guide_wait_state::identity_exhausted;
        return false;
    }
    if (!enabled || ticket.enable_generation != event_enable_generation) {
        failure = guide_wait_state::ineligible;
        return false;
    }
    if (!event_qualification_is_current(position.events, ticket.occupant, ticket.focus_epoch) ||
        !guard.matches_foreground_epoch(ticket.focus_epoch)) {
        failure = guide_wait_state::ineligible;
        return false;
    }
    return true;
}

guide_wait_error service::implementation::guide_wait_error_for_state(
    guide_wait_state state) noexcept {
    switch (state) {
    case guide_wait_state::pending:
    case guide_wait_state::signalled:
        return guide_wait_error::busy;
    case guide_wait_state::cancelled:
        return guide_wait_error::cancelled;
    case guide_wait_state::not_connected:
        return guide_wait_error::not_connected;
    case guide_wait_state::ineligible:
        return guide_wait_error::ineligible;
    case guide_wait_state::history_reset:
        return guide_wait_error::history_reset;
    case guide_wait_state::identity_exhausted:
        return guide_wait_error::identity_exhausted;
    case guide_wait_state::closed:
        return guide_wait_error::closed;
    }
    return guide_wait_error::history_reset;
}

guide_wait_result<guide_wait_ticket> service::begin_guide_wait(DWORD user_index) {
    using guide_wait_state = implementation::guide_wait_state;
    if (user_index >= 4U) {
        return std::unexpected{guide_wait_error::invalid_argument};
    }

    std::optional<read_pin> live_pin;
    std::optional<guide_wait_ticket> created;
    bool notify{};
    {
        const auto guard = implementation_->focus.lock_presentation();
        auto& position = implementation_->positions[user_index];
        auto& events = position.events;
        if (implementation_->closed) {
            return std::unexpected{guide_wait_error::closed};
        }
        if (!position.occupant.has_value()) {
            return std::unexpected{guide_wait_error::not_connected};
        }
        const auto occupant = *position.occupant;
        auto captured = position.reader.capture();
        if (!captured.has_value() || captured->target() != occupant.target) {
            return std::unexpected{guide_wait_error::not_connected};
        }
        live_pin.emplace(std::move(*captured));
        if (implementation_->event_source_provider == nullptr) {
            return std::unexpected{guide_wait_error::not_supported};
        }

        const auto snapshot = guard.published();
        const auto epoch = guard.current_foreground_epoch();
        if (!implementation_->enabled || !epoch.has_value() ||
            !guard.matches_foreground(snapshot) || !guard.matches_foreground_epoch(*epoch)) {
            return std::unexpected{guide_wait_error::ineligible};
        }
        if (events.guide_wait.has_value()) {
            return std::unexpected{guide_wait_error::capacity_exceeded};
        }
        if (implementation_->event_identity_exhausted ||
            implementation_->guide_ticket_ids_exhausted) {
            return std::unexpected{guide_wait_error::identity_exhausted};
        }

        const auto wanted = implementation::event_qualification{.occupant = occupant,
            .focus_epoch = *epoch,
            .enable_generation = implementation_->event_enable_generation};
        if (!events.qualification.has_value() || *events.qualification != wanted) {
            notify =
                implementation::invalidate_event_position(position, guide_wait_state::ineligible);
            events.qualification = wanted;
        }

        const guide_wait_ticket ticket{.user_index = user_index,
            .id = implementation_->next_guide_ticket_id};
        const implementation::guide_wait_record record{.ticket = ticket,
            .occupant = occupant,
            .focus_epoch = *epoch,
            .enable_generation = implementation_->event_enable_generation,
            .domain = events.domain,
            .state = guide_wait_state::pending,
            .awaiter_active = false};
        events.guide_wait = record;
        created = ticket;
        if (implementation_->next_guide_ticket_id == std::numeric_limits<std::uint64_t>::max()) {
            implementation_->guide_ticket_ids_exhausted = true;
        } else {
            ++implementation_->next_guide_ticket_id;
        }
    }
    if (notify) {
        implementation_->notify_guide_waiters();
    }
    return *created;
}

guide_wait_result<void> service::wait_for_guide_button(guide_wait_ticket ticket) {
    using guide_wait_state = implementation::guide_wait_state;
    if (ticket.user_index >= 4U || ticket.id == 0U) {
        return std::unexpected{guide_wait_error::invalid_argument};
    }

    std::unique_lock wait_lock{implementation_->guide_wait_mutex};
    {
        const auto guard = implementation_->focus.lock_presentation();
        auto& record = implementation_->positions[ticket.user_index].events.guide_wait;
        if (!record.has_value() || record->ticket != ticket) {
            return std::unexpected{guide_wait_error::invalid_ticket};
        }
        if (record->awaiter_active) {
            return std::unexpected{guide_wait_error::busy};
        }
        record->awaiter_active = true;
    }

    for (;;) {
        std::optional<read_pin> live_pin;
        bool terminal{};
        guide_wait_state terminal_state{guide_wait_state::pending};
        {
            auto guard = implementation_->focus.lock_presentation();
            auto& position = implementation_->positions[ticket.user_index];
            auto& events = position.events;
            if (!events.guide_wait.has_value() || events.guide_wait->ticket != ticket) {
                return std::unexpected{guide_wait_error::invalid_ticket};
            }
            auto& record = *events.guide_wait;
            if (record.state == guide_wait_state::pending ||
                record.state == guide_wait_state::signalled) {
                guide_wait_state failure{guide_wait_state::ineligible};
                if (!implementation_
                        ->guide_ticket_is_fresh(guard, position, record, live_pin, failure)) {
                    record.state = failure;
                }
            }

            if (record.state != guide_wait_state::pending) {
                terminal = true;
                terminal_state = record.state;
                events.guide_wait.reset();
            }
        }
        if (terminal) {
            if (terminal_state == guide_wait_state::signalled) {
                return {};
            }
            return std::unexpected{implementation::guide_wait_error_for_state(terminal_state)};
        }
        live_pin.reset();
        implementation_->guide_wait_changed.wait_for(wait_lock, qualification_poll_interval);
    }
}

guide_wait_result<bool> service::cancel_guide_wait(guide_wait_ticket ticket) {
    using guide_wait_state = implementation::guide_wait_state;
    if (ticket.user_index >= 4U || ticket.id == 0U) {
        return std::unexpected{guide_wait_error::invalid_argument};
    }

    std::optional<read_pin> live_pin;
    bool cancelled{};
    bool notify{};
    {
        auto guard = implementation_->focus.lock_presentation();
        auto& position = implementation_->positions[ticket.user_index];
        auto& record = position.events.guide_wait;
        if (!record.has_value() || record->ticket != ticket) {
            return std::unexpected{guide_wait_error::invalid_ticket};
        }
        if (record->state == guide_wait_state::pending) {
            guide_wait_state failure{guide_wait_state::ineligible};
            if (implementation_
                    ->guide_ticket_is_fresh(guard, position, *record, live_pin, failure)) {
                record->state = guide_wait_state::cancelled;
                cancelled = true;
            } else {
                record->state = failure;
            }
            notify = true;
        } else if (record->state == guide_wait_state::signalled) {
            guide_wait_state failure{guide_wait_state::ineligible};
            if (!implementation_
                    ->guide_ticket_is_fresh(guard, position, *record, live_pin, failure)) {
                record->state = failure;
                notify = true;
            }
        }
    }
    if (notify) {
        implementation_->notify_guide_waiters();
    }
    return cancelled;
}

guide_wait_result<void> service::release_guide_wait(guide_wait_ticket ticket) {
    using guide_wait_state = implementation::guide_wait_state;
    if (ticket.user_index >= 4U || ticket.id == 0U) {
        return std::unexpected{guide_wait_error::invalid_argument};
    }

    bool notify{};
    {
        const auto guard = implementation_->focus.lock_presentation();
        auto& record = implementation_->positions[ticket.user_index].events.guide_wait;
        if (!record.has_value() || record->ticket != ticket) {
            return std::unexpected{guide_wait_error::invalid_ticket};
        }
        if (record->awaiter_active) {
            return std::unexpected{guide_wait_error::busy};
        }
        if (record->state == guide_wait_state::pending) {
            record->state = guide_wait_state::cancelled;
            notify = true;
        }
        record.reset();
    }
    if (notify) {
        implementation_->notify_guide_waiters();
    }
    return {};
}

} // namespace inputnext::xinput
