#include "publication_source.hpp"

#include "gameinput/history/history_capture.hpp"
#include "gameinput/input_profile.hpp"
#include "gameinput/reading_parts.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace inputnext::gameinput_detail {
namespace {

constexpr button_mask guide_mask = static_cast<button_mask>(gamepad_button::guide);

struct lane_selection final {
    bool pad{};
    bool guide{};
    bool stale_seed{};
};

[[nodiscard]] gamepad_state
merge_state(gamepad_state pad, bool guide_known, bool guide_pressed) noexcept {
    pad.buttons &= ~guide_mask;
    if (guide_known && guide_pressed) {
        pad.buttons |= guide_mask;
    }
    return pad;
}

[[nodiscard]] lane_selection select_lanes(const reading_parts& parts,
    seed_ticket ticket,
    bool is_seed,
    seed_ticket current_epochs) noexcept {
    const bool pad_stale =
        is_seed && parts.pad.has_value() && ticket.pad_epoch != current_epochs.pad_epoch;
    const bool guide_stale =
        is_seed && parts.guide.has_value() && ticket.guide_epoch != current_epochs.guide_epoch;
    return lane_selection{
        .pad = parts.pad.has_value() && is_valid_gamepad_state(*parts.pad) && !pad_stale,
        .guide = parts.guide.has_value() && !guide_stale,
        .stale_seed = pad_stale || guide_stale,
    };
}

} // namespace

publication_source::publication_source(publication_epoch_seed seed,
    input::program mappings,
    native_input_profile profile) noexcept
    : history_(mappings, profile),
      pad_epoch_(seed.pad_epoch),
      guide_epoch_(seed.guide_epoch),
      touch_epoch_(seed.touch_epoch),
      touch_enabled_(mappings.has_touch_routes() && profile.touch_enabled()) {
}

bind_outcome publication_source::bind(producer_binding binding) {
    std::scoped_lock lock{mutex_};
    if (closed_) {
        return bind_outcome::closed;
    }
    if (producer_.has_value()) {
        return bind_outcome::already_bound;
    }
    producer_.emplace(binding);
    return bind_outcome::bound;
}

seed_ticket publication_source::before_seed() const {
    std::scoped_lock lock{mutex_};
    return seed_ticket{.pad_epoch = pad_epoch_,
        .guide_epoch = guide_epoch_,
        .touch_epoch = touch_epoch_,
        .touch_generation = touch_.snapshot().epoch};
}

publication_outcome publication_source::on_callback(const reading_parts& parts) {
    return apply(parts, {}, false);
}

publication_outcome publication_source::on_seed(seed_ticket ticket, const reading_parts& parts) {
    return apply(parts, ticket, true);
}

history_capture& publication_source::history() noexcept {
    return history_;
}

bool publication_source::invalidate_touch_locked(input_fallback_reason reason,
    bool revoke_profile) {
    if (!touch_enabled_) {
        return true;
    }
    auto candidate = touch_;
    const auto resolved = candidate.invalidate(input::touch_loss_reason::missing_click);
    if (pad_known_ && resolved.source != touch_.snapshot().source) {
        const auto physical = merge_state(pad_state_, guide_known_, guide_pressed_);
        const auto result =
            producer_.value_or(producer_binding{})
                .publish(input_update{
                    .state = input::project({.pad = physical, .touch = resolved.source},
                        history_.projection()),
                    .source_timestamp_us = last_source_timestamp_us_,
                    .observation_qpc = last_observation_qpc_,
                });
        if (!result.has_value()) {
            // The old snapshot cannot be neutralized. End the stream without committing
            // a touch state that was never published.
            close_locked();
            return false;
        }
    }
    touch_ = candidate;
    if (revoke_profile) {
        touch_enabled_ = false;
    }
    history_.invalidate_touch(resolved, reason, revoke_profile);
    return true;
}

void publication_source::set_callback_coverage(bool active) {
    std::scoped_lock lock{mutex_};
    if (closed_) {
        return;
    }
    touch_suspended_ = !active;
    if (!active && !invalidate_touch_locked(input_fallback_reason::epoch_changed, false)) {
        return;
    }
    history_.set_callback_coverage(active);
}

publication_outcome
publication_source::apply(const reading_parts& parts, seed_ticket ticket, bool is_seed) {
    std::scoped_lock lock{mutex_};
    if (closed_) {
        return publication_outcome::closed;
    }
    if (fault_ != publication_fault::none) {
        return publication_outcome::epoch_exhausted;
    }
    if (!producer_.has_value()) {
        close_locked();
        return publication_outcome::closed;
    }
    if (parts.observation_qpc == 0U) {
        return publication_outcome::invalid;
    }

    const auto lanes = select_lanes(parts,
        ticket,
        is_seed,
        seed_ticket{.pad_epoch = pad_epoch_, .guide_epoch = guide_epoch_});
    const bool touch_applicable =
        touch_enabled_ && !touch_suspended_ &&
        ((parts.touch.has_value() &&
             parts.touch->click_status != input::observation_status::not_applicable) ||
            lanes.pad);
    const bool stale_touch =
        is_seed && touch_applicable &&
        (ticket.touch_epoch != touch_epoch_ || ticket.touch_generation != touch_.snapshot().epoch);
    bool accept_touch = touch_applicable && !stale_touch;
    if (!is_seed && accept_touch && touch_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        if (!invalidate_touch_locked(input_fallback_reason::epoch_changed, true)) {
            return publication_outcome::closed;
        }
        accept_touch = false;
    }
    if (!lanes.pad && !lanes.guide && !accept_touch) {
        return is_seed && (lanes.stale_seed || stale_touch) ? publication_outcome::stale_seed
                                                            : publication_outcome::invalid;
    }
    if (!is_seed && !advance_callback_epochs(lanes.pad, lanes.guide, accept_touch)) {
        return publication_outcome::epoch_exhausted;
    }
    return publish_selected(parts, lanes.pad, lanes.guide, accept_touch, is_seed);
}

bool publication_source::advance_callback_epochs(bool accept_pad,
    bool accept_guide,
    bool accept_touch) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (accept_pad && pad_epoch_ == maximum) {
        fault_ = publication_fault::pad_epoch_exhausted;
        return false;
    }
    if (accept_guide && guide_epoch_ == maximum) {
        fault_ = publication_fault::guide_epoch_exhausted;
        return false;
    }
    if (accept_pad) {
        ++pad_epoch_;
    }
    if (accept_guide) {
        ++guide_epoch_;
    }
    if (accept_touch) {
        ++touch_epoch_;
    }
    return true;
}

void publication_source::commit_lanes(const reading_parts& parts,
    bool accept_pad,
    bool accept_guide) noexcept {
    if (accept_pad) {
        pad_state_ = parts.pad.value_or(pad_state_);
        pad_known_ = true;
    }
    if (accept_guide) {
        guide_pressed_ = parts.guide.value_or(guide_pressed_);
        guide_known_ = true;
    }
}

publication_outcome publication_source::publish_selected(const reading_parts& parts,
    bool accept_pad,
    bool accept_guide,
    bool accept_touch,
    bool is_seed) {
    auto next_touch = touch_;
    if (accept_touch) {
        auto observation = parts.touch.value_or(input::touch_observation{});
        // A claimed touch observation without its required pad view cannot create a source.
        if ((!accept_pad || observation.source_timestamp != parts.source_timestamp_us) &&
            observation.click_status == input::observation_status::observed) {
            observation.click_status = input::observation_status::unavailable;
        }
        observation.source_timestamp = parts.source_timestamp_us;
        static_cast<void>(next_touch.observe(observation));
    }
    if (!pad_known_ && !accept_pad) {
        commit_lanes(parts, false, accept_guide);
        touch_ = next_touch;
        capture_accepted(parts, false, accept_guide, accept_touch, is_seed);
        return publication_outcome::partial_only;
    }

    const auto next_pad = accept_pad ? parts.pad.value_or(pad_state_) : pad_state_;
    const bool next_guide_known = guide_known_ || accept_guide;
    const bool next_guide_pressed =
        accept_guide ? parts.guide.value_or(guide_pressed_) : guide_pressed_;
    const input_update update{
        .state = input::project({.pad = merge_state(next_pad, next_guide_known, next_guide_pressed),
                                    .touch = next_touch.snapshot().source},
            history_.projection()),
        .source_timestamp_us = parts.source_timestamp_us,
        .observation_qpc = parts.observation_qpc,
    };
    const auto published = producer_.value_or(producer_binding{}).publish(update);
    if (!published.has_value()) {
        return map_publication_failure(published.error());
    }

    commit_lanes(parts, accept_pad, accept_guide);
    touch_ = next_touch;
    last_source_timestamp_us_ = parts.source_timestamp_us;
    last_observation_qpc_ = parts.observation_qpc;
    capture_accepted(parts, accept_pad, accept_guide, accept_touch, is_seed);
    return publication_outcome::published;
}

void publication_source::capture_accepted(const reading_parts& parts,
    bool accept_pad,
    bool accept_guide,
    bool accept_touch,
    bool is_seed) {
    auto accepted = parts;
    if (!accept_pad) {
        accepted.pad.reset();
    }
    if (!accept_guide) {
        accepted.guide.reset();
    }
    accepted.resolved_touch = accept_touch ? std::optional{touch_.snapshot()} : std::nullopt;
    if (accept_touch) {
        const auto reason = touch_.snapshot().reason;
        const bool incoherent =
            reason == input::touch_loss_reason::incoherent_reading ||
            (parts.touch.has_value() &&
                parts.touch->click_status == input::observation_status::observed &&
                (!accept_pad || parts.touch->source_timestamp != parts.source_timestamp_us));
        if (incoherent) {
            history_.record_fallback(input_fallback_reason::incoherent_reading);
        } else if (reason == input::touch_loss_reason::missing_click) {
            history_.record_fallback(input_fallback_reason::missing_click);
        } else if (reason == input::touch_loss_reason::invalid_coordinates ||
                   reason == input::touch_loss_reason::incomplete_contacts) {
            history_.record_fallback(input_fallback_reason::invalid_coordinates);
        } else if (reason == input::touch_loss_reason::unsupported_capability) {
            history_.record_fallback(input_fallback_reason::unsupported_capability);
        }
    }
    if (is_seed) {
        history_.on_seed(accepted);
    } else {
        history_.on_callback(accepted);
    }
}

publication_outcome publication_source::map_publication_failure(error failure) noexcept {
    if (failure == error::revision_exhausted) {
        return publication_outcome::revision_exhausted;
    }
    if (failure == error::not_connected) {
        close_locked();
        return publication_outcome::closed;
    }
    return publication_outcome::invalid;
}

void publication_source::close() {
    std::scoped_lock lock{mutex_};
    close_locked();
}

void publication_source::close_locked() {
    closed_ = true;
    history_.close();
}

source_status publication_source::status() const {
    std::scoped_lock lock{mutex_};
    return source_status{
        .closed = closed_,
        .pad_known = pad_known_,
        .guide_known = guide_known_,
        .fault = fault_,
        .pad_epoch = pad_epoch_,
        .guide_epoch = guide_epoch_,
        .touch = touch_.snapshot(),
    };
}

} // namespace inputnext::gameinput_detail
