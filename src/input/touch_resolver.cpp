#include "touch_resolver.hpp"
#include "program.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace inputnext::input {
namespace {

void advance_epoch(touch_snapshot& state) noexcept {
    auto& epoch = state.epoch;
    if (epoch != std::numeric_limits<std::uint64_t>::max()) {
        ++epoch;
    } else {
        state.epoch_exhausted = true;
    }
}

[[nodiscard]] bool valid_coordinates(const touch_observation& observation) noexcept {
    if (!observation.same_reading || observation.contacts_status != observation_status::observed ||
        !observation.contacts_complete || observation.contact_count == 0U ||
        observation.contact_count > observation.contacts.size() ||
        !std::isfinite(observation.surface_width) || !std::isfinite(observation.surface_height) ||
        observation.surface_width <= 0.0F || observation.surface_height <= 0.0F) {
        return false;
    }

    for (std::size_t index = 0U; index < observation.contact_count; ++index) {
        const auto& contact = observation.contacts[index];
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (observation.contacts[earlier].id == contact.id) {
                return false;
            }
        }
        if (!contact.active) {
            continue;
        }
        if (!std::isfinite(contact.x) || !std::isfinite(contact.y) || contact.x < 0.0F ||
            contact.y < 0.0F || contact.x >= observation.surface_width ||
            contact.y >= observation.surface_height) {
            return false;
        }
    }

    return std::any_of(observation.contacts.begin(),
        observation.contacts.begin() + static_cast<std::ptrdiff_t>(observation.contact_count),
        [](const touch_contact& contact) { return contact.active; });
}

} // namespace

touch_snapshot touch_resolver::observe(const touch_observation& observation) noexcept {
    if (observation.click_status == observation_status::not_applicable ||
        observation.click_status == observation_status::unchanged) {
        return state_;
    }
    if (observation.click_status == observation_status::unsupported) {
        return invalidate(touch_loss_reason::unsupported_capability);
    }
    if (observation.click_status == observation_status::unavailable) {
        return invalidate(touch_loss_reason::missing_click);
    }

    if (!state_.first_accepted_timestamp.has_value()) {
        state_.first_accepted_timestamp = observation.source_timestamp;
    }
    if (!observation.click_pressed) {
        has_click_baseline_ = true;
        release();
        return state_;
    }

    const bool was_pressed = state_.click_known_pressed;
    const bool had_baseline = has_click_baseline_;
    has_click_baseline_ = true;
    state_.click_known_pressed = true;
    if (state_.awaiting_release || state_.epoch_exhausted) {
        state_.source = touch_source::none;
        state_.reason = touch_loss_reason::none;
        return state_;
    }
    if (was_pressed) {
        return state_;
    }

    if (!had_baseline) {
        state_.source = touch_source::fallback;
        state_.reason = touch_loss_reason::initial_click_held;
        return state_;
    }
    state_.source = classify(observation);
    if (state_.source != touch_source::fallback) {
        state_.reason = touch_loss_reason::none;
    } else if (observation.contacts_status == observation_status::unsupported) {
        state_.reason = touch_loss_reason::unsupported_capability;
    } else if (!observation.same_reading) {
        state_.reason = touch_loss_reason::incoherent_reading;
    } else if (observation.contacts_status != observation_status::observed ||
               !observation.contacts_complete || observation.contact_count == 0U ||
               observation.contact_count > observation.contacts.size()) {
        state_.reason = touch_loss_reason::incomplete_contacts;
    } else {
        state_.reason = touch_loss_reason::invalid_coordinates;
    }
    return state_;
}

touch_snapshot touch_resolver::invalidate(const touch_loss_reason reason) noexcept {
    if (!state_.awaiting_release) {
        advance_epoch(state_);
        state_.first_accepted_timestamp.reset();
    }
    state_.source = touch_source::none;
    state_.click_known_pressed = false;
    state_.awaiting_release = true;
    state_.reason = reason;
    return state_;
}

touch_snapshot touch_resolver::reset() noexcept {
    advance_epoch(state_);
    state_.source = touch_source::none;
    state_.click_known_pressed = false;
    state_.awaiting_release = false;
    state_.first_accepted_timestamp.reset();
    state_.reason = touch_loss_reason::none;
    has_click_baseline_ = false;
    return state_;
}

touch_snapshot touch_resolver::snapshot() const noexcept {
    return state_;
}

void touch_resolver::release() noexcept {
    state_.source = touch_source::none;
    state_.click_known_pressed = false;
    state_.awaiting_release = false;
    state_.reason = touch_loss_reason::none;
}

touch_source touch_resolver::classify(const touch_observation& observation) noexcept {
    if (!valid_coordinates(observation)) {
        return touch_source::fallback;
    }
    const touch_contact* first = nullptr;
    for (std::size_t index = 0U; index < observation.contact_count; ++index) {
        const auto& contact = observation.contacts[index];
        if (contact.active && (first == nullptr || contact.id < first->id)) {
            first = &contact;
        }
    }
    if (first == nullptr) {
        return touch_source::fallback;
    }
    return first->x < observation.surface_width / 2.0 ? touch_source::left : touch_source::right;
}

} // namespace inputnext::input
