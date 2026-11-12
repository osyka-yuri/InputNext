#include "native_history_cursor.hpp"

#include "gameinput/history/clock_mapping.hpp"
#include "gameinput/input_profile.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "native_gameinput.hpp"
#include "native_history.hpp"
#include "native_history_api.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] native_history_step clock_discontinuity() noexcept {
    return native_history_step{.kind = native_history_step_kind::clock_discontinuity};
}

[[nodiscard]] native_failure clock_failure(std::uint32_t native_code = 0U) noexcept {
    return native_failure{
        .cause = native_failure_cause::clock_discontinuity,
        .native_code = native_code,
    };
}

struct initial_clock_sample final {
    history_clock_anchor anchor{};
    std::uint64_t source_timestamp_us{};
    std::uint64_t qpc_frequency{};
};

[[nodiscard]] native_result<initial_clock_sample> sample_initial_clock(
    native_history_api& api) noexcept {
    const auto frequency = api.query_qpc_frequency();
    if (!frequency.has_value()) {
        return std::unexpected(clock_failure(frequency.error().native_code));
    }
    if (*frequency == 0U) {
        return std::unexpected(clock_failure());
    }

    const auto qpc_before = api.query_qpc();
    if (!qpc_before.has_value()) {
        return std::unexpected(clock_failure(qpc_before.error().native_code));
    }
    if (*qpc_before == 0U) {
        return std::unexpected(clock_failure());
    }

    const auto source_anchor = api.current_timestamp();
    const auto qpc_after = api.query_qpc();
    if (!qpc_after.has_value()) {
        return std::unexpected(clock_failure(qpc_after.error().native_code));
    }
    if (*qpc_after < *qpc_before) {
        return std::unexpected(clock_failure());
    }

    return initial_clock_sample{
        .anchor =
            history_clock_anchor{
                .source_timestamp_us = source_anchor,
                .qpc = *qpc_before,
                .qpc_frequency = *frequency,
            },
        .source_timestamp_us = source_anchor,
        .qpc_frequency = *frequency,
    };
}

[[nodiscard]] native_result<std::uint64_t> map_initial_reading(native_history_api& api,
    const initial_clock_sample& sample,
    const std::uint64_t reading_timestamp) noexcept {
    const auto mapped_reading = map_history_timestamp(sample.anchor, reading_timestamp);
    const auto capture_qpc = api.query_qpc();
    if (!mapped_reading.has_value()) {
        return std::unexpected(clock_failure());
    }
    if (!capture_qpc.has_value()) {
        return std::unexpected(clock_failure(capture_qpc.error().native_code));
    }
    if (*capture_qpc == 0U) {
        return std::unexpected(clock_failure());
    }

    return (std::max)(mapped_reading->qpc, *capture_qpc);
}

[[nodiscard]] native_failure bootstrap_failure(input_fallback_reason reason,
    native_failure_cause cause = native_failure_cause::native_failure,
    std::uint32_t native_code = 0U) noexcept {
    return native_failure{
        .cause = cause,
        .native_code = native_code,
        .input_reason = reason,
    };
}

[[nodiscard]] input_fallback_reason touch_fallback_reason(
    const input::touch_snapshot& snapshot) noexcept {
    switch (snapshot.reason) {
    case input::touch_loss_reason::none:
        return input_fallback_reason::none;
    case input::touch_loss_reason::initial_click_held:
        return input_fallback_reason::incomplete_replay;
    case input::touch_loss_reason::missing_click:
        return input_fallback_reason::missing_click;
    case input::touch_loss_reason::incoherent_reading:
        return input_fallback_reason::incoherent_reading;
    case input::touch_loss_reason::incomplete_contacts:
    case input::touch_loss_reason::invalid_coordinates:
        return input_fallback_reason::invalid_coordinates;
    case input::touch_loss_reason::unsupported_capability:
        return input_fallback_reason::unsupported_capability;
    }
    return input_fallback_reason::incoherent_reading;
}

} // namespace

native_history_cursor_impl::reading_reference::reading_reference(native_history_api& api,
    void* reading) noexcept
    : api_(&api),
      reading_(reading) {
}

native_history_cursor_impl::reading_reference::~reading_reference() {
    reset();
}

native_history_cursor_impl::reading_reference::reading_reference(reading_reference&& other) noexcept
    : api_(std::exchange(other.api_, nullptr)),
      reading_(std::exchange(other.reading_, nullptr)) {
}

native_history_cursor_impl::reading_reference&
native_history_cursor_impl::reading_reference::operator=(reading_reference&& other) noexcept {
    if (this != &other) {
        reset();
        api_ = std::exchange(other.api_, nullptr);
        reading_ = std::exchange(other.reading_, nullptr);
    }
    return *this;
}

void* native_history_cursor_impl::reading_reference::get() const noexcept {
    return reading_;
}

void native_history_cursor_impl::reading_reference::reset() noexcept {
    if (api_ != nullptr && reading_ != nullptr) {
        api_->release_reading(reading_);
    }
    api_ = nullptr;
    reading_ = nullptr;
}

void native_history_cursor_impl::reading_reference::reset(native_history_api& api,
    void* reading) noexcept {
    reset();
    if (reading != nullptr) {
        api_ = &api;
        reading_ = reading;
    }
}

native_history_cursor_impl::native_history_cursor_impl(std::unique_ptr<native_history_api> api,
    native_device_handle device,
    native_input_profile profile,
    input::touch_snapshot touch_context) noexcept
    : api_(std::move(api)),
      device_(device),
      profile_(profile),
      touch_context_(touch_context) {
    if (api_ != nullptr && device_.valid()) {
        api_->retain_session(device_);
        session_retained_ = true;
    }
}

native_history_cursor_impl::~native_history_cursor_impl() {
    reference_.reset();
    if (session_retained_) {
        api_->release_session(device_);
    }
}

std::uint32_t native_history_cursor_impl::bootstrap_transitions() const noexcept {
    return bootstrap_transitions_;
}

native_result<native_history_seed> native_history_cursor_impl::start() noexcept {
    bootstrap_transitions_ = 0U;
    reference_.reset();
    initialized_ = false;
    checkpoint_qpc_ = 0U;
    if (api_ == nullptr || !session_retained_) {
        return std::unexpected(native_failure{});
    }

    const auto composite = profile_.history == input_history_path::composite;
    if (composite && !profile_.touch_enabled()) {
        return std::unexpected(native_failure{
            .cause = native_failure_cause::api_unavailable,
            .input_reason = input_fallback_reason::unsupported_capability,
        });
    }
    const auto start_clock = sample_initial_clock(*api_);
    if (!start_clock.has_value()) {
        return std::unexpected(start_clock.error());
    }
    const auto current =
        composite ? api_->current_composite(device_, profile_) : api_->current_pad(device_);
    reading_reference current_reading{*api_, current.reading};
    if (current.kind != native_history_read_kind::reading || current_reading.get() == nullptr) {
        const auto cause = current.kind == native_history_read_kind::disconnected
                               ? native_failure_cause::device_disconnected
                               : native_failure_cause::no_current_reading;
        return std::unexpected(native_failure{.cause = cause, .native_code = current.native_code});
    }

    const auto source_timestamp = api_->reading_timestamp(current_reading.get());
    gamepad_state state{};
    native_history_frame composite_frame{};
    if (composite) {
        if (!read_composite_frame(current_reading.get(), composite_frame)) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
                .native_code = current.native_code,
                .input_reason = last_composite_failure_reason_,
            });
        }
        state = composite_frame.pad;
    } else {
        auto pad = api_->read_gamepad(current_reading.get());
        if (!pad.has_value()) {
            return std::unexpected(pad.error());
        }
        state = *pad;
        if (!is_valid_gamepad_state(state)) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
                .native_code = current.native_code,
            });
        }
    }

    const auto mapped_reading = map_initial_reading(*api_, *start_clock, source_timestamp);
    if (!mapped_reading.has_value()) {
        return std::unexpected(mapped_reading.error());
    }
    const auto baseline_qpc = *mapped_reading;

    native_history_seed seed{};
    if (composite) {
        auto composite_seed = start_composite(start_clock->qpc_frequency,
            current_reading.get(),
            source_timestamp,
            baseline_qpc,
            composite_frame);
        if (!composite_seed.has_value()) {
            return std::unexpected(composite_seed.error());
        }
        seed = *composite_seed;
    } else {
        static_cast<void>(touch_resolver_.reset());
        seed = native_history_seed{
            .baseline =
                input_update{
                    .state = state,
                    .source_timestamp_us = source_timestamp,
                    .observation_qpc = baseline_qpc,
                },
            .qpc_frequency = start_clock->qpc_frequency,
        };
    }

    reference_ = std::move(current_reading);
    anchor_ = start_clock->anchor;
    last_observed_timestamp_us_ = start_clock->source_timestamp_us;
    last_reading_timestamp_us_ = source_timestamp;
    timeline_floor_qpc_ = baseline_qpc;
    checkpoint_qpc_ = 0U;
    initialized_ = baseline_qpc != 0U;
    if (!initialized_) {
        reference_.reset();
        return std::unexpected(clock_failure());
    }

    return seed;
}

native_result<native_history_seed> native_history_cursor_impl::start_composite(
    const std::uint64_t qpc_frequency,
    void* current_reading,
    const std::uint64_t current_timestamp,
    const std::uint64_t baseline_qpc,
    const native_history_frame& current_frame) noexcept {
    last_composite_failure_reason_ = input_fallback_reason::none;
    static_cast<void>(touch_resolver_.reset());

    if (!current_frame.touch.click_pressed) {
        const auto resolved = touch_resolver_.observe(current_frame.touch);
        return native_history_seed{
            .baseline =
                input_update{
                    .state = current_frame.pad,
                    .source_timestamp_us = current_timestamp,
                    .observation_qpc = baseline_qpc,
                },
            .qpc_frequency = qpc_frequency,
            .touch = resolved.source,
            .resolved_touch = resolved,
        };
    }
    if (!touch_context_.first_accepted_timestamp.has_value() || touch_context_.awaiting_release) {
        last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
        return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay));
    }

    auto reset_point =
        find_released_reset(current_reading, *touch_context_.first_accepted_timestamp);
    if (!reset_point.has_value()) {
        return std::unexpected(reset_point.error());
    }
    auto replay = replay_from_reset(std::move(*reset_point), current_reading, current_frame);
    if (!replay.has_value()) {
        return std::unexpected(replay.error());
    }

    return native_history_seed{
        .baseline =
            input_update{
                .state = current_frame.pad,
                .source_timestamp_us = current_timestamp,
                .observation_qpc = baseline_qpc,
            },
        .qpc_frequency = qpc_frequency,
        .touch = replay->resolved.source,
        .resolved_touch = replay->resolved,
        .bootstrap_transitions = replay->transitions,
    };
}

native_result<native_history_cursor_impl::bootstrap_reset_point>
native_history_cursor_impl::find_released_reset(void* current_reading,
    const std::uint64_t first_accepted_timestamp) noexcept {
    constexpr std::uint32_t bootstrap_limit = 256U;
    reading_reference walk_reading;
    auto* cursor = current_reading;
    auto newer_timestamp = api_->reading_timestamp(current_reading);

    while (bootstrap_transitions_ < bootstrap_limit) {
        const auto previous = api_->previous_composite(device_, cursor, profile_);
        if (previous.kind != native_history_read_kind::reading || previous.reading == nullptr) {
            if (previous.kind == native_history_read_kind::disconnected) {
                last_composite_failure_reason_ = input_fallback_reason::none;
                return std::unexpected(native_failure{
                    .cause = native_failure_cause::device_disconnected,
                    .native_code = previous.native_code,
                });
            }
            const auto reason = previous.kind == native_history_read_kind::reference_too_old ||
                                        previous.kind == native_history_read_kind::no_reading
                                    ? input_fallback_reason::history_lost
                                    : input_fallback_reason::incomplete_replay;
            last_composite_failure_reason_ = reason;
            return std::unexpected(bootstrap_failure(reason,
                native_failure_cause::native_failure,
                previous.native_code));
        }
        ++bootstrap_transitions_;

        reading_reference candidate{*api_, previous.reading};
        if (api_->same_reading(candidate.get(), cursor)) {
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay));
        }
        const auto candidate_timestamp = api_->reading_timestamp(candidate.get());
        if (candidate_timestamp > newer_timestamp) {
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay,
                native_failure_cause::clock_discontinuity));
        }

        native_history_frame candidate_frame{};
        if (!read_composite_frame(candidate.get(), candidate_frame)) {
            return std::unexpected(bootstrap_failure(last_composite_failure_reason_));
        }
        if (!candidate_frame.touch.click_pressed &&
            candidate_timestamp > first_accepted_timestamp) {
            return bootstrap_reset_point{
                .reading = std::move(candidate),
                .frame = candidate_frame,
            };
        }

        newer_timestamp = candidate_timestamp;
        walk_reading = std::move(candidate);
        cursor = walk_reading.get();
    }

    last_composite_failure_reason_ = input_fallback_reason::bootstrap_budget;
    return std::unexpected(bootstrap_failure(input_fallback_reason::bootstrap_budget));
}

native_result<native_history_cursor_impl::bootstrap_replay_result>
native_history_cursor_impl::replay_from_reset(bootstrap_reset_point reset_point,
    void* current_reading,
    const native_history_frame& current_frame) noexcept {
    constexpr std::uint32_t bootstrap_limit = 256U;
    auto resolved = touch_resolver_.observe(reset_point.frame.touch);
    if (resolved.click_known_pressed || resolved.source != input::touch_source::none) {
        last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
        return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay));
    }

    auto replay_timestamp = api_->reading_timestamp(reset_point.reading.get());
    for (;;) {
        if (api_->same_reading(reset_point.reading.get(), current_reading)) {
            break;
        }
        if (bootstrap_transitions_ >= bootstrap_limit) {
            last_composite_failure_reason_ = input_fallback_reason::bootstrap_budget;
            return std::unexpected(bootstrap_failure(input_fallback_reason::bootstrap_budget));
        }

        const auto next = api_->next_composite(device_, reset_point.reading.get(), profile_);
        if (next.kind != native_history_read_kind::reading || next.reading == nullptr) {
            if (next.kind == native_history_read_kind::disconnected) {
                last_composite_failure_reason_ = input_fallback_reason::none;
                return std::unexpected(native_failure{
                    .cause = native_failure_cause::device_disconnected,
                    .native_code = next.native_code,
                });
            }
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay,
                native_failure_cause::native_failure,
                next.native_code));
        }
        ++bootstrap_transitions_;
        reading_reference next_reading{*api_, next.reading};
        if (api_->same_reading(reset_point.reading.get(), next_reading.get())) {
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay));
        }
        const auto next_timestamp = api_->reading_timestamp(next_reading.get());
        if (next_timestamp < replay_timestamp) {
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay,
                native_failure_cause::clock_discontinuity));
        }

        native_history_frame frame{};
        if (!read_composite_frame(next_reading.get(), frame)) {
            return std::unexpected(bootstrap_failure(last_composite_failure_reason_));
        }
        resolved = touch_resolver_.observe(frame.touch);
        if (api_->same_reading(next_reading.get(), current_reading) &&
            frame.touch.click_pressed != current_frame.touch.click_pressed) {
            last_composite_failure_reason_ = input_fallback_reason::incomplete_replay;
            return std::unexpected(bootstrap_failure(input_fallback_reason::incomplete_replay));
        }

        replay_timestamp = next_timestamp;
        reset_point.reading.reset();
        reset_point.reading = std::move(next_reading);
    }

    return bootstrap_replay_result{
        .resolved = resolved,
        .transitions = bootstrap_transitions_,
    };
}

bool native_history_cursor_impl::read_composite_frame(void* reading,
    native_history_frame& frame) noexcept {
    auto result = api_->read_composite(reading, profile_);
    if (!result.has_value()) {
        last_composite_failure_reason_ = input_fallback_reason::incoherent_reading;
        return false;
    }
    if (!is_valid_gamepad_state(result->pad)) {
        last_composite_failure_reason_ = input_fallback_reason::incoherent_reading;
        return false;
    }
    if (result->touch.click_status != input::observation_status::observed) {
        last_composite_failure_reason_ = input_fallback_reason::missing_click;
        return false;
    }
    if (!result->touch.same_reading ||
        result->touch.source_timestamp != api_->reading_timestamp(reading)) {
        last_composite_failure_reason_ = input_fallback_reason::incoherent_reading;
        return false;
    }
    frame = *result;
    return true;
}

native_history_step native_history_cursor_impl::failure_step(native_history_read read) noexcept {
    if (read.reading != nullptr) {
        const reading_reference unexpected_reading{*api_, read.reading};
    }

    switch (read.kind) {
    case native_history_read_kind::reference_too_old:
        return native_history_step{
            .kind = native_history_step_kind::history_lost,
            .native_code = read.native_code,
            .fallback_reason = input_fallback_reason::history_lost,
        };
    case native_history_read_kind::disconnected:
        return native_history_step{
            .kind = native_history_step_kind::disconnected,
            .native_code = read.native_code,
        };
    case native_history_read_kind::reading:
    case native_history_read_kind::no_reading:
    case native_history_read_kind::failure:
        return native_history_step{
            .kind = native_history_step_kind::provider_failure,
            .native_code = read.native_code,
        };
    }
    return native_history_step{
        .kind = native_history_step_kind::provider_failure,
        .native_code = read.native_code,
    };
}

native_history_step native_history_cursor_impl::next() noexcept {
    if (!initialized_ || api_ == nullptr || reference_.get() == nullptr) {
        return native_history_step{.kind = native_history_step_kind::provider_failure};
    }

    const auto observed_timestamp = api_->current_timestamp();
    if (observed_timestamp < last_observed_timestamp_us_) {
        return clock_discontinuity();
    }
    last_observed_timestamp_us_ = observed_timestamp;

    const bool composite = profile_.history == input_history_path::composite;
    const auto next_reading = composite ? api_->next_composite(device_, reference_.get(), profile_)
                                        : api_->next_pad(device_, reference_.get());
    if (next_reading.kind == native_history_read_kind::no_reading) {
        return caught_up(next_reading, observed_timestamp);
    }
    if (next_reading.kind != native_history_read_kind::reading || next_reading.reading == nullptr) {
        return failure_step(next_reading);
    }

    return process_next_reading(next_reading);
}

native_history_step native_history_cursor_impl::caught_up(native_history_read read,
    const std::uint64_t observed_timestamp) noexcept {
    if (read.reading != nullptr) {
        const reading_reference unexpected_reading{*api_, read.reading};
    }
    const auto mapped_observation =
        map_history_timestamp(anchor_, observed_timestamp, timeline_floor_qpc_);
    if (!mapped_observation.has_value()) {
        return clock_discontinuity();
    }
    checkpoint_qpc_ = (std::max)(timeline_floor_qpc_, mapped_observation->qpc);
    timeline_floor_qpc_ = checkpoint_qpc_;
    return native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = checkpoint_qpc_,
    };
}

native_history_step native_history_cursor_impl::process_next_reading(
    const native_history_read read) noexcept {
    reading_reference reading{*api_, read.reading};
    const bool composite = profile_.history == input_history_path::composite;
    const auto source_timestamp = api_->reading_timestamp(reading.get());
    if (source_timestamp < last_reading_timestamp_us_) {
        return clock_discontinuity();
    }

    const auto mapped = map_history_timestamp(anchor_, source_timestamp, timeline_floor_qpc_);
    if (!mapped.has_value()) {
        return clock_discontinuity();
    }

    gamepad_state state{};
    input::touch_snapshot resolved_touch{};
    input_fallback_reason fallback_reason{input_fallback_reason::none};
    if (composite) {
        native_history_frame frame{};
        if (!read_composite_frame(reading.get(), frame)) {
            return native_history_step{
                .kind = native_history_step_kind::incomplete_replay,
                .native_code = read.native_code,
                .fallback_reason = last_composite_failure_reason_,
            };
        }
        state = frame.pad;
        resolved_touch = touch_resolver_.observe(frame.touch);
        fallback_reason = touch_fallback_reason(resolved_touch);
    } else {
        auto pad = api_->read_gamepad(reading.get());
        if (!pad.has_value() || !is_valid_gamepad_state(*pad)) {
            const auto failure = pad.has_value()
                                     ? native_failure{.cause = native_failure_cause::native_failure}
                                     : pad.error();
            return native_history_step{
                .kind = native_history_step_kind::provider_failure,
                .native_code = failure.native_code,
            };
        }
        state = *pad;
    }

    last_reading_timestamp_us_ = source_timestamp;
    timeline_floor_qpc_ = mapped->qpc;
    reference_ = std::move(reading);
    return native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = state,
                .source_timestamp_us = source_timestamp,
                .observation_qpc = mapped->qpc,
            },
        .checkpoint_qpc = checkpoint_qpc_,
        .touch = resolved_touch.source,
        .resolved_touch = resolved_touch,
        .fallback_reason = fallback_reason,
    };
}

} // namespace inputnext::gameinput_detail
