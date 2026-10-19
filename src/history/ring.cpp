#include "ring.hpp"

#include <inputnext/history.hpp>

#include <cstdint>
#include <limits>

namespace inputnext::history_detail {

bool history_ring::append(ring_entry entry, std::uint64_t& position) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (next_position_ == maximum) {
        return false;
    }

    auto next_loss_mark = tail_loss_mark_;
    auto creates_unknown_cut = false;
    if (entry.kind == ring_entry_kind::reading) {
        if (next_loss_mark.known_prefix == maximum) {
            creates_unknown_cut = true;
        } else {
            ++next_loss_mark.known_prefix;
        }
    } else if (entry.kind == ring_entry_kind::recovery_baseline) {
        if (entry.loss == history_loss::unknown) {
            creates_unknown_cut = true;
        } else if (entry.loss == history_loss::known) {
            if (entry.lost_count > maximum - next_loss_mark.known_prefix) {
                creates_unknown_cut = true;
            } else {
                next_loss_mark.known_prefix += entry.lost_count;
            }
        }
    }
    if (creates_unknown_cut) {
        next_loss_mark = {};
    }

    if (next_position_ - oldest_position_ == history_bank_capacity) {
        ++oldest_position_;
    }

    position = next_position_;
    entry.position = next_position_;
    entry.loss_mark = next_loss_mark;
    entries_[index_of(next_position_)] = entry;
    tail_loss_mark_ = next_loss_mark;
    if (creates_unknown_cut) {
        latest_unknown_cut_ = next_position_;
    }
    ++next_position_;
    return true;
}

const ring_entry* history_ring::find(std::uint64_t position) const noexcept {
    if (position < oldest_position_ || position >= next_position_) {
        return nullptr;
    }

    const auto& entry = entries_[index_of(position)];
    return entry.position == position ? &entry : nullptr;
}

const ring_entry* history_ring::latest() const noexcept {
    return empty() ? nullptr : find(next_position_ - 1U);
}

ring_overrun history_ring::overrun(std::uint64_t next_position,
    ring_loss_mark before_next) const noexcept {
    if (next_position >= oldest_position_) {
        return {.loss = history_loss::none};
    }

    const auto* latest_entry = latest();
    if (latest_entry == nullptr || latest_unknown_cut_ >= next_position) {
        return {.loss = history_loss::unknown};
    }

    const auto excluded_latest = latest_entry->kind == ring_entry_kind::reading ? 1U : 0U;
    if (latest_entry->loss_mark.known_prefix < before_next.known_prefix ||
        latest_entry->loss_mark.known_prefix - before_next.known_prefix < excluded_latest) {
        return {.loss = history_loss::unknown};
    }

    return ring_overrun{.loss = history_loss::known,
        .lost_count =
            latest_entry->loss_mark.known_prefix - before_next.known_prefix - excluded_latest};
}

} // namespace inputnext::history_detail
