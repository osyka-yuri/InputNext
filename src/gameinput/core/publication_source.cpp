#include "publication_source.hpp"

#include "gameinput/history/history_capture.hpp"
#include "gameinput/reading_parts.hpp"
#include <inputnext/error.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>

#include <cstdint>
#include <limits>
#include <mutex>

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

publication_source::publication_source(publication_epoch_seed seed) noexcept
    : pad_epoch_(seed.pad_epoch),
      guide_epoch_(seed.guide_epoch) {
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
    return seed_ticket{.pad_epoch = pad_epoch_, .guide_epoch = guide_epoch_};
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
        closed_ = true;
        history_.close();
        return publication_outcome::closed;
    }
    if (parts.observation_qpc == 0U) {
        return publication_outcome::invalid;
    }

    const auto lanes = select_lanes(parts,
        ticket,
        is_seed,
        seed_ticket{.pad_epoch = pad_epoch_, .guide_epoch = guide_epoch_});
    if (!lanes.pad && !lanes.guide) {
        return is_seed && lanes.stale_seed ? publication_outcome::stale_seed
                                           : publication_outcome::invalid;
    }
    if (!is_seed && !advance_callback_epochs(lanes.pad, lanes.guide)) {
        return publication_outcome::epoch_exhausted;
    }
    return publish_selected(parts, lanes.pad, lanes.guide, is_seed);
}

bool publication_source::advance_callback_epochs(bool accept_pad, bool accept_guide) noexcept {
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
    bool is_seed) {
    if (!pad_known_ && !accept_pad) {
        commit_lanes(parts, false, true);
        auto accepted = parts;
        accepted.pad.reset();
        if (is_seed) {
            history_.on_seed(accepted);
        } else {
            history_.on_callback(accepted);
        }
        return publication_outcome::partial_only;
    }

    const auto next_pad = accept_pad ? parts.pad.value_or(pad_state_) : pad_state_;
    const bool next_guide_known = guide_known_ || accept_guide;
    const bool next_guide_pressed =
        accept_guide ? parts.guide.value_or(guide_pressed_) : guide_pressed_;
    const input_update update{
        .state = merge_state(next_pad, next_guide_known, next_guide_pressed),
        .source_timestamp_us = parts.source_timestamp_us,
        .observation_qpc = parts.observation_qpc,
    };
    const auto published = producer_.value_or(producer_binding{}).publish(update);
    if (!published.has_value()) {
        return map_publication_failure(published.error());
    }

    commit_lanes(parts, accept_pad, accept_guide);
    auto accepted = parts;
    if (!accept_pad) {
        accepted.pad.reset();
    }
    if (!accept_guide) {
        accepted.guide.reset();
    }
    if (is_seed) {
        history_.on_seed(accepted);
    } else {
        history_.on_callback(accepted);
    }
    return publication_outcome::published;
}

publication_outcome publication_source::map_publication_failure(error failure) noexcept {
    if (failure == error::revision_exhausted) {
        return publication_outcome::revision_exhausted;
    }
    if (failure == error::not_connected) {
        closed_ = true;
        history_.close();
        return publication_outcome::closed;
    }
    return publication_outcome::invalid;
}

void publication_source::close() {
    std::scoped_lock lock{mutex_};
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
    };
}

} // namespace inputnext::gameinput_detail
