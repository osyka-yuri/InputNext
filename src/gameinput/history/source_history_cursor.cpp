#include "source_history_cursor.hpp"

#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "history/cursor.hpp"
#include "history_capture.hpp"
#include "history_capture_log.hpp"
#include "input/program.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

using history_detail::cursor_step_kind;

constexpr button_mask guide_mask = static_cast<button_mask>(gamepad_button::guide);
constexpr auto native_retry_delay = std::chrono::seconds{1};

[[nodiscard]] gamepad_state without_guide(gamepad_state state) noexcept {
    state.buttons &= ~guide_mask;
    return state;
}

[[nodiscard]] gamepad_state
merge_guide(gamepad_state pad, bool guide_known, bool guide_pressed) noexcept {
    pad = without_guide(pad);
    if (guide_known && guide_pressed) {
        pad.buttons |= guide_mask;
    }
    return pad;
}

[[nodiscard]] bool guide_pressed(const gamepad_state& state) noexcept {
    return (state.buttons & guide_mask) != 0U;
}

[[nodiscard]] history_failure to_history_failure(const native_failure& failure) noexcept {
    history_error code = history_error::provider_failure;
    switch (failure.cause) {
    case native_failure_cause::device_disconnected:
        code = history_error::disconnected;
        break;
    case native_failure_cause::no_current_reading:
        code = history_error::no_data;
        break;
    case native_failure_cause::resource_exhausted:
        code = history_error::capacity_exceeded;
        break;
    case native_failure_cause::clock_discontinuity:
        code = history_error::clock_discontinuity;
        break;
    case native_failure_cause::load_failed:
    case native_failure_cause::api_unavailable:
    case native_failure_cause::initialization_failed:
    case native_failure_cause::registration_failed:
    case native_failure_cause::native_failure:
        break;
    }
    return history_failure{.code = code, .native_code = failure.native_code};
}

[[nodiscard]] history_failure failure(history_error code, std::uint32_t native_code = 0U) noexcept {
    return history_failure{.code = code, .native_code = native_code};
}

[[nodiscard]] std::uint64_t max_qpc(std::uint64_t left, std::uint64_t right) noexcept {
    return (std::max)(left, right);
}

[[nodiscard]] bool guide_precedes_step(const capture_record& guide,
    const native_history_step& native_step) noexcept {
    const auto guide_qpc = guide.sample.effective_qpc;
    const auto native_qpc = native_step.kind == native_history_step_kind::reading
                                ? native_step.reading.observation_qpc
                                : native_step.checkpoint_qpc;
    const bool inclusive = guide.kind == capture_record_kind::guide_cut ||
                           guide.kind == capture_record_kind::touch_cut ||
                           native_step.kind == native_history_step_kind::caught_up;
    return inclusive ? guide_qpc <= native_qpc : guide_qpc < native_qpc;
}

} // namespace

source_history_cursor::source_history_cursor(native_gameinput& native,
    native_device_handle device,
    history_capture& capture)
    : native_(native),
      device_(device),
      capture_(capture) {
}

history_result<history_detail::cursor_seed> source_history_cursor::start(
    history_time now) noexcept {
    static_cast<void>(now);
    reset_progress();
    if (!device_.valid()) {
        return std::unexpected(failure(history_error::invalid_argument));
    }

    const auto snapshot = capture_.snapshot();
    if (snapshot.closed) {
        return std::unexpected(failure(history_error::disconnected));
    }
    input_profile_ = snapshot.input_profile;
    touch_epoch_ = snapshot.touch.epoch;
    if (input_profile_.history == input_history_path::callback_capture) {
        return start_fallback(snapshot,
            native_failure{.cause = native_failure_cause::api_unavailable});
    }
    if (input_profile_.touch_enabled() &&
        (!snapshot.callback_coverage || !snapshot.touch.first_accepted_timestamp.has_value())) {
        return start_without_native(snapshot,
            native_failure{.cause = native_failure_cause::no_current_reading,
                .input_reason = input_fallback_reason::incomplete_replay});
    }

    auto created = create_native_cursor();
    if (!created.has_value()) {
        return start_without_native(capture_.snapshot(), created.error());
    }

    native_cursor_ = std::move(*created);
    const auto seed = native_cursor_->start();
    if (!seed.has_value()) {
        const auto native_failure_value = seed.error();
        native_cursor_.reset();
        return start_without_native(capture_.snapshot(), native_failure_value);
    }

    const auto after = capture_.snapshot();
    if (input_profile_.touch_enabled() &&
        (!same_touch_epoch(after) || !after.callback_coverage ||
            snapshot.coverage_generation != after.coverage_generation)) {
        native_cursor_.reset();
        return start_without_native(after,
            native_failure{.cause = native_failure_cause::no_current_reading,
                .input_reason = input_fallback_reason::epoch_changed});
    }
    return start_native(*seed, after);
}

native_result<std::unique_ptr<native_history_cursor>>
source_history_cursor::create_native_cursor() noexcept {
    try {
        const auto snapshot = capture_.snapshot();
        auto created =
            native_.create_history_cursor(device_, snapshot.input_profile, snapshot.touch);
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        if (*created == nullptr) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::resource_exhausted,
            });
        }
        return created;
    } catch (const std::bad_alloc&) {
        return std::unexpected(native_failure{
            .cause = native_failure_cause::resource_exhausted,
        });
    } catch (...) {
        return std::unexpected(native_failure{});
    }
}

history_result<history_detail::cursor_seed> source_history_cursor::start_without_native(
    const capture_snapshot& snapshot,
    native_failure native_failure_value) noexcept {
    if (native_failure_value.cause == native_failure_cause::device_disconnected) {
        return std::unexpected(to_history_failure(native_failure_value));
    }
    capture_.record_fallback(native_failure_value.input_reason);
    return start_fallback(snapshot, native_failure_value);
}

history_result<history_detail::cursor_seed> source_history_cursor::start_native(
    const native_history_seed& seed,
    const capture_snapshot& snapshot) noexcept {
    if (snapshot.closed) {
        native_cursor_.reset();
        return std::unexpected(failure(history_error::disconnected));
    }
    if (seed.qpc_frequency == 0U || seed.baseline.observation_qpc == 0U) {
        native_cursor_.reset();
        return start_fallback(snapshot,
            native_failure{.cause = native_failure_cause::clock_discontinuity});
    }

    const auto guide_fact = snapshot.guide_fact.value_or(history_sample{});
    const bool has_fresh_guide =
        snapshot.guide_fresh && snapshot.guide_fact.has_value() && guide_fact.guide_known;
    guide_known_ = has_fresh_guide;
    guide_pressed_ = has_fresh_guide && guide_pressed(guide_fact.state);
    guide_fresh_ = snapshot.guide_fresh;
    callback_coverage_ = snapshot.callback_coverage;
    coverage_generation_ = snapshot.coverage_generation;
    current_pad_ = without_guide(seed.baseline.state);
    current_touch_ = input_profile_.touch_enabled() ? seed.touch : input::touch_source::none;
    auto baseline_qpc = seed.baseline.observation_qpc;
    auto baseline_source_timestamp = seed.baseline.source_timestamp_us;
    if (has_fresh_guide) {
        baseline_qpc = max_qpc(baseline_qpc, guide_fact.effective_qpc);
        baseline_source_timestamp =
            (std::max)(baseline_source_timestamp, guide_fact.source_timestamp_us);
    }
    timeline_floor_qpc_ = baseline_qpc;
    capture_position_ = snapshot.next_position;
    capture_position_exhausted_ = snapshot.exhausted;
    fallback_mode_ = false;
    initialized_ = true;
    capture_.record_selected_history(input_profile_.history);
    return history_detail::cursor_seed{
        .baseline =
            history_sample{
                .state = logical_state(guide_known_),
                .source_timestamp_us = baseline_source_timestamp,
                .effective_qpc = baseline_qpc,
                .guide_known = guide_known_,
            },
        .qpc_frequency = seed.qpc_frequency,
        .source = history_source_kind::native_history,
    };
}

history_result<history_detail::cursor_seed> source_history_cursor::start_fallback(
    const capture_snapshot& snapshot,
    native_failure native_failure_value) noexcept {
    if (snapshot.closed) {
        return std::unexpected(failure(history_error::disconnected));
    }
    const auto baseline = snapshot.fallback_baseline.value_or(history_sample{});
    if (!snapshot.fallback_baseline.has_value() || baseline.effective_qpc == 0U) {
        return std::unexpected(to_history_failure(native_failure_value));
    }

    const auto frequency = native_.performance_frequency();
    if (!frequency.has_value()) {
        return std::unexpected(to_history_failure(frequency.error()));
    }
    const auto qpc_frequency = *frequency;
    if (qpc_frequency == 0U) {
        return std::unexpected(failure(history_error::clock_discontinuity));
    }

    const auto current_snapshot = capture_.snapshot();
    if (current_snapshot.closed) {
        return std::unexpected(failure(history_error::disconnected));
    }
    if (!same_touch_epoch(current_snapshot)) {
        capture_.record_fallback(input_fallback_reason::epoch_changed);
    }
    const auto current_baseline = current_snapshot.fallback_baseline;
    if (!current_baseline.has_value() || current_baseline->effective_qpc == 0U) {
        return std::unexpected(to_history_failure(native_failure_value));
    }

    current_pad_ = without_guide(current_baseline->state);
    input_profile_ = current_snapshot.input_profile;
    current_touch_ =
        input_profile_.touch_enabled() ? current_snapshot.touch.source : input::touch_source::none;
    touch_epoch_ = current_snapshot.touch.epoch;
    guide_known_ = current_baseline->guide_known;
    guide_pressed_ = guide_known_ && guide_pressed(current_baseline->state);
    guide_fresh_ = current_snapshot.guide_fresh;
    callback_coverage_ = current_snapshot.callback_coverage;
    coverage_generation_ = current_snapshot.coverage_generation;
    timeline_floor_qpc_ = current_baseline->effective_qpc;
    capture_position_ = current_snapshot.next_position;
    capture_position_exhausted_ = current_snapshot.exhausted;
    fallback_mode_ = true;
    initialized_ = true;
    capture_.record_selected_history(input_history_path::callback_capture);
    if (!capture_position_exhausted_ &&
        input_profile_.history != input_history_path::callback_capture) {
        schedule_native_retry(current_time());
    }

    return history_detail::cursor_seed{
        .baseline = history_sample{.state = logical_state(guide_known_),
            .source_timestamp_us = current_baseline->source_timestamp_us,
            .effective_qpc = current_baseline->effective_qpc,
            .guide_known = current_baseline->guide_known},
        .qpc_frequency = qpc_frequency,
        .source = history_source_kind::callback_capture,
    };
}

history_detail::cursor_step source_history_cursor::next(history_time now) noexcept {
    if (!initialized_) {
        return make_step(cursor_step_kind::provider_failure);
    }
    if (capture_position_exhausted_) {
        return make_step(cursor_step_kind::exhausted);
    }
    capture_snapshot snapshot{};
    const auto context = read_capture_context(snapshot);
    if (context != capture_context_status::current) {
        return make_step(context == capture_context_status::closed
                             ? cursor_step_kind::disconnected
                             : cursor_step_kind::history_lost);
    }
    return fallback_mode_ ? next_fallback(now) : next_native();
}

history_detail::cursor_step source_history_cursor::next_native() noexcept {
    if (!pending_guide_.has_value()) {
        const auto capture_step = read_native_capture();
        if (capture_step.has_value()) {
            return *capture_step;
        }
    }

    if (capture_position_exhausted_ && !pending_guide_.has_value()) {
        return make_step(cursor_step_kind::exhausted);
    }
    return merge_native_step();
}

std::optional<history_detail::cursor_step> source_history_cursor::read_native_capture() noexcept {
    const auto captured = capture_.read(capture_position_);
    switch (captured.kind) {
    case capture_read_kind::record:
        advance_capture_position();
        if (captured.record.kind == capture_record_kind::guide ||
            captured.record.kind == capture_record_kind::guide_cut ||
            captured.record.kind == capture_record_kind::touch_cut) {
            pending_guide_ = captured.record;
            return std::nullopt;
        }
        return make_step(cursor_step_kind::progress);
    case capture_read_kind::end:
        return std::nullopt;
    case capture_read_kind::overwritten:
        if (captured.overwritten_control_position >= capture_position_) {
            return make_step(cursor_step_kind::history_lost);
        }
        if (captured.oldest_position > capture_position_) {
            capture_position_ = captured.oldest_position;
            return make_step(cursor_step_kind::progress);
        }
        return make_step(cursor_step_kind::history_lost);
    case capture_read_kind::closed:
        return make_step(cursor_step_kind::disconnected);
    case capture_read_kind::exhausted:
        capture_position_exhausted_ = true;
        return make_step(cursor_step_kind::exhausted);
    }
    return make_step(cursor_step_kind::provider_failure);
}

std::optional<history_detail::cursor_step> source_history_cursor::prepare_native_step() noexcept {
    if (!pending_native_.has_value()) {
        if (native_cursor_ == nullptr) {
            return make_step(cursor_step_kind::provider_failure);
        }
        capture_snapshot before{};
        const auto before_context = read_capture_context(before);
        if (before_context != capture_context_status::current) {
            return make_step(before_context == capture_context_status::closed
                                 ? cursor_step_kind::disconnected
                                 : cursor_step_kind::history_lost);
        }
        pending_native_ = native_cursor_->next();
        capture_snapshot after{};
        const auto after_context = read_capture_context(after);
        if (after_context != capture_context_status::current) {
            return make_step(after_context == capture_context_status::closed
                                 ? cursor_step_kind::disconnected
                                 : cursor_step_kind::history_lost);
        }
        if (before.callback_coverage != after.callback_coverage ||
            before.coverage_generation != after.coverage_generation) {
            return make_step(cursor_step_kind::progress);
        }
    } else {
        capture_snapshot snapshot{};
        const auto context = read_capture_context(snapshot);
        if (context != capture_context_status::current) {
            return make_step(context == capture_context_status::closed
                                 ? cursor_step_kind::disconnected
                                 : cursor_step_kind::history_lost);
        }
    }
    return std::nullopt;
}

history_detail::cursor_step source_history_cursor::merge_native_step() noexcept {
    const auto prepared = prepare_native_step();
    if (prepared.has_value()) {
        return *prepared;
    }

    const auto native_step = pending_native_.value_or(native_history_step{});
    const auto native_kind = native_step.kind;
    if (native_kind != native_history_step_kind::reading &&
        native_kind != native_history_step_kind::caught_up) {
        if (pending_guide_.has_value()) {
            return emit_guide();
        }
        capture_.record_fallback(native_step.fallback_reason);
        const auto step = failure_step(native_step);
        pending_native_.reset();
        return step;
    }

    if (pending_guide_.has_value() && guide_precedes_step(*pending_guide_, native_step)) {
        return emit_guide();
    }

    if (native_kind == native_history_step_kind::reading) {
        const auto step = emit_native(native_step);
        pending_native_.reset();
        return step;
    }

    const auto checkpoint = native_step.checkpoint_qpc;
    pending_native_.reset();
    if (checkpoint < timeline_floor_qpc_ || !caught_up_is_qualified()) {
        auto step = make_step(cursor_step_kind::progress);
        step.checkpoint_qpc = 0U;
        return step;
    }

    timeline_floor_qpc_ = max_qpc(timeline_floor_qpc_, checkpoint);
    return history_detail::cursor_step{
        .kind = cursor_step_kind::caught_up,
        .checkpoint_qpc = timeline_floor_qpc_,
    };
}

history_detail::cursor_step source_history_cursor::next_fallback(history_time now) noexcept {
    if (promotion_pending_) {
        if (capture_position_ >= promotion_end_position_) {
            return make_step(cursor_step_kind::history_lost);
        }
    } else if (native_retry_at_.has_value() && now >= *native_retry_at_) {
        return try_native_promotion();
    }

    const auto captured = capture_.read(capture_position_);
    capture_snapshot snapshot{};
    const auto context = read_capture_context(snapshot);
    if (context != capture_context_status::current) {
        return make_step(context == capture_context_status::closed
                             ? cursor_step_kind::disconnected
                             : cursor_step_kind::history_lost);
    }
    switch (captured.kind) {
    case capture_read_kind::record:
        if (promotion_pending_ && captured.record.position >= promotion_end_position_) {
            return make_step(cursor_step_kind::history_lost);
        }
        advance_capture_position();
        return emit_fallback(captured.record);
    case capture_read_kind::end:
        return fallback_checkpoint();
    case capture_read_kind::overwritten: {
        if (captured.oldest_position <= capture_position_) {
            return make_step(cursor_step_kind::history_lost);
        }
        capture_position_ = captured.oldest_position;
        return make_step(cursor_step_kind::history_lost);
    }
    case capture_read_kind::closed:
        return make_step(cursor_step_kind::disconnected);
    case capture_read_kind::exhausted:
        capture_position_exhausted_ = true;
        return make_step(cursor_step_kind::exhausted);
    }
    return make_step(cursor_step_kind::provider_failure);
}

history_detail::cursor_step source_history_cursor::fallback_checkpoint() noexcept {
    const auto snapshot = capture_.snapshot();
    if (snapshot.closed) {
        return make_step(cursor_step_kind::disconnected);
    }
    if (snapshot.exhausted) {
        capture_position_exhausted_ = true;
        return make_step(cursor_step_kind::exhausted);
    }
    if (!promotion_pending_ && snapshot.next_position > capture_position_) {
        return make_step(cursor_step_kind::progress);
    }
    callback_coverage_ = snapshot.callback_coverage;
    guide_fresh_ = snapshot.guide_fresh;
    coverage_generation_ = snapshot.coverage_generation;
    if (promotion_pending_) {
        return make_step(cursor_step_kind::progress);
    }
    if (!callback_coverage_ || !guide_fresh_) {
        auto step = make_step(cursor_step_kind::progress);
        step.checkpoint_qpc = 0U;
        return step;
    }
    return history_detail::cursor_step{
        .kind = cursor_step_kind::caught_up,
        .checkpoint_qpc = timeline_floor_qpc_,
    };
}

history_detail::cursor_step source_history_cursor::try_native_promotion() noexcept {
    const auto before = capture_.snapshot();
    auto created = create_native_cursor();
    auto native_failure_value = native_failure{};
    if (!created.has_value()) {
        native_failure_value = created.error();
    } else {
        const auto probe = std::move(*created);
        const auto started = probe->start();
        if (!started.has_value()) {
            native_failure_value = started.error();
        }
        if (started.has_value()) {
            const auto snapshot = capture_.snapshot();
            if (snapshot.closed) {
                return make_step(cursor_step_kind::disconnected);
            }
            if (snapshot.exhausted) {
                native_retry_at_.reset();
                return make_step(cursor_step_kind::progress);
            }
            if (before.input_profile.touch_enabled() &&
                (!same_touch_epoch(snapshot) || !snapshot.callback_coverage ||
                    before.coverage_generation != snapshot.coverage_generation)) {
                capture_.record_fallback(input_fallback_reason::epoch_changed);
                schedule_native_retry(current_time());
                return make_step(cursor_step_kind::progress);
            }
            promotion_end_position_ = snapshot.next_position;
            promotion_pending_ = true;
            native_retry_at_.reset();
            return make_step(cursor_step_kind::progress);
        }
    }
    if (native_failure_value.cause == native_failure_cause::device_disconnected) {
        return failure_step(native_failure_value);
    }
    capture_.record_fallback(native_failure_value.input_reason);

    schedule_native_retry(current_time());
    return make_step(cursor_step_kind::progress,
        native_diagnostic{.code = native_failure_value.native_code});
}

history_detail::cursor_step source_history_cursor::emit_guide() noexcept {
    if (!pending_guide_.has_value()) {
        return make_step(cursor_step_kind::progress);
    }
    const auto record = *pending_guide_;
    pending_guide_.reset();
    if (record.kind == capture_record_kind::guide_cut ||
        record.kind == capture_record_kind::touch_cut) {
        return make_step(cursor_step_kind::history_lost);
    }

    guide_pressed_ = guide_pressed(record.sample.state);
    guide_known_ = record.sample.guide_known;
    guide_fresh_ = guide_known_;
    callback_coverage_ = true;
    return emit_reading(record.sample, history_record_origin::callback_guide, guide_known_);
}

history_detail::cursor_step source_history_cursor::emit_native(
    const native_history_step& step) noexcept {
    const auto reading = step.reading;
    current_pad_ = without_guide(reading.state);
    current_touch_ = input_profile_.touch_enabled() ? step.touch : input::touch_source::none;
    const bool guide_is_current = callback_coverage_ && guide_fresh_ && guide_known_;
    return emit_reading({.source_timestamp_us = reading.source_timestamp_us,
                            .effective_qpc = reading.observation_qpc},
        history_record_origin::native_pad,
        guide_is_current);
}

history_detail::cursor_step source_history_cursor::emit_fallback(
    const capture_record& record) noexcept {
    if (record.kind == capture_record_kind::guide_cut ||
        record.kind == capture_record_kind::fallback_cut ||
        record.kind == capture_record_kind::touch_cut) {
        return make_step(cursor_step_kind::history_lost);
    }
    current_touch_ = record.touch;

    if (record.kind == capture_record_kind::guide) {
        guide_known_ = record.sample.guide_known;
        guide_pressed_ = guide_known_ && guide_pressed(record.sample.state);
        guide_fresh_ = guide_known_;
        callback_coverage_ = true;
    } else {
        current_pad_ = without_guide(record.sample.state);
        guide_known_ = record.sample.guide_known;
        guide_pressed_ = guide_known_ && guide_pressed(record.sample.state);
    }
    const auto origin = record.kind == capture_record_kind::guide
                            ? history_record_origin::callback_guide
                            : history_record_origin::callback_pad;
    return emit_reading(record.sample, origin, guide_known_);
}

history_detail::cursor_step source_history_cursor::emit_reading(history_sample reading,
    history_record_origin origin,
    bool guide_known) noexcept {
    timeline_floor_qpc_ = max_qpc(reading.effective_qpc, timeline_floor_qpc_);
    reading.state = logical_state(guide_known);
    reading.effective_qpc = timeline_floor_qpc_;
    reading.guide_known = guide_known;
    return history_detail::cursor_step{
        .kind = cursor_step_kind::reading,
        .reading = reading,
        .origin = origin,
    };
}

history_detail::cursor_step source_history_cursor::make_step(cursor_step_kind kind) noexcept {
    return make_step(kind, native_diagnostic{});
}

history_detail::cursor_step source_history_cursor::make_step(cursor_step_kind kind,
    native_diagnostic diagnostic) noexcept {
    auto step = history_detail::cursor_step{
        .kind = kind,
        .native_code = diagnostic.code,
    };
    if (kind == cursor_step_kind::history_lost) {
        step.loss = history_loss::unknown;
    }
    return step;
}

history_detail::cursor_step source_history_cursor::failure_step(
    const native_failure& failure_value) noexcept {
    auto kind = cursor_step_kind::provider_failure;
    if (failure_value.cause == native_failure_cause::device_disconnected) {
        kind = cursor_step_kind::disconnected;
    } else if (failure_value.cause == native_failure_cause::clock_discontinuity) {
        kind = cursor_step_kind::clock_discontinuity;
    }
    return make_step(kind, native_diagnostic{.code = failure_value.native_code});
}

history_detail::cursor_step source_history_cursor::failure_step(
    const native_history_step& step) noexcept {
    switch (step.kind) {
    case native_history_step_kind::provider_failure:
        return make_step(cursor_step_kind::provider_failure,
            native_diagnostic{.code = step.native_code});
    case native_history_step_kind::clock_discontinuity:
        return make_step(cursor_step_kind::clock_discontinuity,
            native_diagnostic{.code = step.native_code});
    case native_history_step_kind::disconnected:
        return make_step(cursor_step_kind::disconnected,
            native_diagnostic{.code = step.native_code});
    case native_history_step_kind::history_lost:
    case native_history_step_kind::incomplete_replay:
    case native_history_step_kind::bootstrap_budget:
        return make_step(cursor_step_kind::history_lost,
            native_diagnostic{.code = step.native_code});
    case native_history_step_kind::exhausted:
        return make_step(cursor_step_kind::exhausted, native_diagnostic{.code = step.native_code});
    case native_history_step_kind::reading:
    case native_history_step_kind::caught_up:
        break;
    }
    return make_step(cursor_step_kind::provider_failure,
        native_diagnostic{.code = step.native_code});
}

void source_history_cursor::advance_capture_position() noexcept {
    if (capture_position_ == std::numeric_limits<std::uint64_t>::max()) {
        capture_position_exhausted_ = true;
        return;
    }
    ++capture_position_;
}

bool source_history_cursor::caught_up_is_qualified() const noexcept {
    if (!callback_coverage_ || !guide_fresh_ || !guide_known_ || capture_position_exhausted_) {
        return false;
    }
    const auto snapshot = capture_.snapshot();
    return !snapshot.closed && same_touch_epoch(snapshot) && snapshot.callback_coverage &&
           snapshot.guide_fresh && snapshot.coverage_generation == coverage_generation_ &&
           snapshot.next_position == capture_position_;
}

source_history_cursor::capture_context_status source_history_cursor::read_capture_context(
    capture_snapshot& snapshot) noexcept {
    snapshot = capture_.snapshot();
    if (snapshot.closed) {
        return capture_context_status::closed;
    }
    if (same_touch_epoch(snapshot)) {
        return capture_context_status::current;
    }
    capture_.record_fallback(input_fallback_reason::epoch_changed);
    pending_native_.reset();
    pending_guide_.reset();
    return capture_context_status::epoch_changed;
}

gamepad_state source_history_cursor::logical_state(bool guide_known) const noexcept {
    return input::project(
        {.pad = merge_guide(current_pad_, guide_known, guide_pressed_), .touch = current_touch_},
        capture_.projection());
}

bool source_history_cursor::same_touch_epoch(const capture_snapshot& snapshot) const noexcept {
    return snapshot.input_profile == input_profile_ && snapshot.touch.epoch == touch_epoch_;
}

void source_history_cursor::reset_progress() noexcept {
    native_cursor_.reset();
    pending_native_.reset();
    pending_guide_.reset();
    native_retry_at_.reset();
    capture_position_ = 1U;
    promotion_end_position_ = 0U;
    coverage_generation_ = 0U;
    timeline_floor_qpc_ = 0U;
    current_pad_ = {};
    current_touch_ = input::touch_source::none;
    input_profile_ = {};
    touch_epoch_ = 0U;
    fallback_mode_ = false;
    callback_coverage_ = false;
    guide_fresh_ = false;
    guide_known_ = false;
    guide_pressed_ = false;
    initialized_ = false;
    promotion_pending_ = false;
    capture_position_exhausted_ = false;
}

void source_history_cursor::schedule_native_retry(history_time completion) noexcept {
    constexpr auto maximum = history_time::max();
    if (completion > maximum - native_retry_delay) {
        native_retry_at_.reset();
        return;
    }
    native_retry_at_ = completion + native_retry_delay;
}

history_time source_history_cursor::current_time() const noexcept {
    return clock_ != nullptr ? clock_() : steady_clock_now();
}

history_time source_history_cursor::steady_clock_now() noexcept {
    return std::chrono::steady_clock::now();
}

} // namespace inputnext::gameinput_detail
