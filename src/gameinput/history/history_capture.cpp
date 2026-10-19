#include "history_capture.hpp"

#include "gameinput/reading_parts.hpp"
#include "history_capture_log.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace inputnext::gameinput_detail {
namespace {

constexpr button_mask guide_mask = static_cast<button_mask>(gamepad_button::guide);

[[nodiscard]] gamepad_state
merge_guide(gamepad_state pad, bool guide_known, bool guide_pressed) noexcept {
    pad.buttons &= ~guide_mask;
    if (guide_known && guide_pressed) {
        pad.buttons |= guide_mask;
    }
    return pad;
}

} // namespace

void history_capture::append(capture_record_kind kind,
    const gamepad_state& pad,
    bool guide_known,
    bool guide_pressed,
    std::uint64_t source_timestamp_us,
    std::uint64_t qpc) noexcept {
    log_.append(kind,
        history_sample{
            .state = merge_guide(pad, guide_known, guide_pressed),
            .source_timestamp_us = source_timestamp_us,
            .effective_qpc = qpc,
            .guide_known = guide_known,
        });
}

void history_capture::refresh_fallback_baseline(std::uint64_t source_timestamp_us,
    std::uint64_t qpc) noexcept {
    if (!pad_known_) {
        fallback_baseline_.reset();
        return;
    }

    const bool known_guide = guide_fact_.has_value() && guide_fresh_;
    std::uint64_t baseline_qpc = (std::max)(pad_qpc_, qpc);
    std::uint64_t baseline_source_timestamp = (std::max)(pad_timestamp_us_, source_timestamp_us);
    if (known_guide) {
        baseline_qpc = (std::max)(baseline_qpc, guide_fact_->effective_qpc);
        baseline_source_timestamp =
            (std::max)(baseline_source_timestamp, guide_fact_->source_timestamp_us);
    }
    fallback_baseline_ = history_sample{
        .state = merge_guide(pad_, known_guide, guide_pressed_),
        .source_timestamp_us = baseline_source_timestamp,
        .effective_qpc = baseline_qpc,
        .guide_known = known_guide,
    };
}

void history_capture::append_cut(capture_record_kind kind,
    std::uint64_t source_timestamp_us,
    std::uint64_t qpc) noexcept {
    append(kind,
        pad_known_ ? pad_ : gamepad_state{},
        guide_fact_.has_value() && guide_fresh_,
        guide_pressed_,
        source_timestamp_us,
        qpc);
}

void history_capture::update_pad(const reading_parts& accepted, bool fallback_cut) noexcept {
    if (!accepted.pad.has_value() || !is_valid_gamepad_state(*accepted.pad)) {
        return;
    }

    const bool changed =
        !pad_known_ || pad_ != *accepted.pad || pad_timestamp_us_ != accepted.source_timestamp_us;
    pad_ = *accepted.pad;
    pad_.buttons &= ~guide_mask;
    pad_known_ = true;
    pad_timestamp_us_ = accepted.source_timestamp_us;
    pad_qpc_ = accepted.observation_qpc;
    refresh_fallback_baseline(accepted.source_timestamp_us, accepted.observation_qpc);

    if (fallback_cut && changed) {
        append(capture_record_kind::fallback_cut,
            pad_,
            guide_fact_.has_value() && guide_fresh_,
            guide_pressed_,
            accepted.source_timestamp_us,
            accepted.observation_qpc);
    }
}

void history_capture::update_guide_callback(const reading_parts& accepted) noexcept {
    if (!accepted.guide.has_value()) {
        return;
    }

    guide_pressed_ = *accepted.guide;
    guide_fresh_ = callback_coverage_;
    guide_fact_ = history_sample{
        .state = merge_guide(pad_known_ ? pad_ : gamepad_state{}, true, guide_pressed_),
        .source_timestamp_us = accepted.source_timestamp_us,
        .effective_qpc = accepted.observation_qpc,
        .guide_known = true,
    };

    if (callback_coverage_) {
        append(capture_record_kind::guide,
            pad_known_ ? pad_ : gamepad_state{},
            true,
            guide_pressed_,
            accepted.source_timestamp_us,
            accepted.observation_qpc);
    }
    refresh_fallback_baseline(accepted.source_timestamp_us, accepted.observation_qpc);
}

void history_capture::update_guide_seed(const reading_parts& accepted) noexcept {
    if (!accepted.guide.has_value()) {
        return;
    }

    const bool changed = !guide_fact_.has_value() || guide_pressed_ != *accepted.guide;
    const bool needs_cut = changed || !guide_fresh_;
    guide_pressed_ = *accepted.guide;
    guide_fresh_ = true;
    guide_fact_ = history_sample{
        .state = merge_guide(pad_known_ ? pad_ : gamepad_state{}, true, guide_pressed_),
        .source_timestamp_us = accepted.source_timestamp_us,
        .effective_qpc = accepted.observation_qpc,
        .guide_known = true,
    };

    if (needs_cut) {
        append_cut(capture_record_kind::guide_cut,
            accepted.source_timestamp_us,
            accepted.observation_qpc);
    }
    refresh_fallback_baseline(accepted.source_timestamp_us, accepted.observation_qpc);
}

void history_capture::on_callback(const reading_parts& accepted) noexcept {
    std::scoped_lock lock{mutex_};
    if (closed_ || log_.exhausted() || accepted.observation_qpc == 0U) {
        return;
    }

    if (accepted.pad.has_value() && is_valid_gamepad_state(*accepted.pad)) {
        update_pad(accepted, false);
        if (callback_coverage_) {
            append(capture_record_kind::pad,
                pad_,
                guide_fact_.has_value() && guide_fresh_,
                guide_pressed_,
                accepted.source_timestamp_us,
                accepted.observation_qpc);
        }
    }

    update_guide_callback(accepted);
    if (!callback_coverage_) {
        refresh_fallback_baseline(accepted.source_timestamp_us, accepted.observation_qpc);
    }
}

void history_capture::on_seed(const reading_parts& accepted) noexcept {
    std::scoped_lock lock{mutex_};
    if (closed_ || log_.exhausted() || accepted.observation_qpc == 0U) {
        return;
    }

    if (accepted.pad.has_value() && is_valid_gamepad_state(*accepted.pad)) {
        update_pad(accepted, true);
    }
    update_guide_seed(accepted);
    refresh_fallback_baseline(accepted.source_timestamp_us, accepted.observation_qpc);
}

void history_capture::set_callback_coverage(bool active) noexcept {
    std::scoped_lock lock{mutex_};
    if (closed_ || callback_coverage_ == active) {
        return;
    }

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (coverage_generation_ == maximum) {
        callback_coverage_ = active;
        guide_fresh_ = false;
        fallback_baseline_.reset();
        log_.exhaust();
        return;
    }

    callback_coverage_ = active;
    ++coverage_generation_;
    guide_fresh_ = false;

    const auto cut = guide_fact_.value_or(history_sample{});
    append(capture_record_kind::guide_cut,
        pad_known_ ? pad_ : gamepad_state{},
        false,
        guide_pressed_,
        cut.source_timestamp_us,
        cut.effective_qpc);
    refresh_fallback_baseline(cut.source_timestamp_us, cut.effective_qpc);
}

void history_capture::close() noexcept {
    std::scoped_lock lock{mutex_};
    if (closed_) {
        return;
    }
    callback_coverage_ = false;
    guide_fresh_ = false;
    closed_ = true;
    log_.close();
}

capture_snapshot history_capture::snapshot() const noexcept {
    std::scoped_lock lock{mutex_};
    return capture_snapshot{
        .fallback_baseline = fallback_baseline_,
        .guide_fact = guide_fact_,
        .next_position = log_.next_position(),
        .oldest_position = log_.oldest_position(),
        .tail_position = log_.tail_position(),
        .overwritten_control_position = log_.overwritten_control_position(),
        .coverage_generation = coverage_generation_,
        .callback_coverage = callback_coverage_,
        .guide_fresh = guide_fresh_,
        .closed = closed_,
        .exhausted = log_.exhausted(),
    };
}

capture_read history_capture::read(std::uint64_t position) const noexcept {
    std::scoped_lock lock{mutex_};
    return log_.read(position);
}

} // namespace inputnext::gameinput_detail
