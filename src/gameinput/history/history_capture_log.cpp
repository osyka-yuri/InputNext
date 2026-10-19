#include "history_capture_log.hpp"

#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace inputnext::gameinput_detail {

std::uint64_t history_capture_log::next_position() const noexcept {
    return next_position_;
}

std::uint64_t history_capture_log::oldest_position() const noexcept {
    return size_ == 0U ? next_position_ : tail_position_ - (size_ - 1U);
}

std::uint64_t history_capture_log::tail_position() const noexcept {
    return tail_position_;
}

std::uint64_t history_capture_log::overwritten_control_position() const noexcept {
    return overwritten_control_position_;
}

bool history_capture_log::exhausted() const noexcept {
    return exhausted_;
}

bool history_capture_log::closed() const noexcept {
    return closed_;
}

bool history_capture_log::is_control(capture_record_kind kind) noexcept {
    return kind == capture_record_kind::guide || kind == capture_record_kind::guide_cut;
}

void history_capture_log::append(capture_record_kind kind, const history_sample& sample) noexcept {
    if (closed_ || exhausted_) {
        return;
    }

    const auto position = next_position_;
    const auto slot = static_cast<std::size_t>(position % history_capture_capacity);
    if (size_ == history_capture_capacity) {
        const auto& overwritten = records_[slot];
        if (is_control(overwritten.kind)) {
            overwritten_control_position_ = overwritten.position;
        }
    } else {
        ++size_;
    }

    records_[slot] = capture_record{.kind = kind, .position = position, .sample = sample};
    tail_position_ = position;
    if (position == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
        return;
    }
    ++next_position_;
}

void history_capture_log::close() noexcept {
    closed_ = true;
}

void history_capture_log::exhaust() noexcept {
    exhausted_ = true;
}

capture_read history_capture_log::read(std::uint64_t position) const noexcept {
    capture_read result{
        .oldest_position = oldest_position(),
        .tail_position = tail_position(),
        .overwritten_control_position = overwritten_control_position_,
    };
    if (closed_) {
        result.kind = capture_read_kind::closed;
        return result;
    }
    if (position < result.oldest_position) {
        result.kind = capture_read_kind::overwritten;
        return result;
    }
    if (position <= result.tail_position) {
        const auto& record =
            records_[static_cast<std::size_t>(position % history_capture_capacity)];
        if (record.position == position) {
            result.kind = capture_read_kind::record;
            result.record = record;
            return result;
        }
        result.kind = capture_read_kind::overwritten;
        return result;
    }
    result.kind = exhausted_ ? capture_read_kind::exhausted : capture_read_kind::end;
    return result;
}

} // namespace inputnext::gameinput_detail
