#include "native_history_cursor.hpp"

#include "gameinput/history/clock_mapping.hpp"
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
    native_device_handle device) noexcept
    : api_(std::move(api)),
      device_(device) {
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

native_result<native_history_seed> native_history_cursor_impl::start() noexcept {
    reference_.reset();
    initialized_ = false;
    checkpoint_qpc_ = 0U;
    if (api_ == nullptr || !session_retained_) {
        return std::unexpected(native_failure{});
    }

    const auto frequency = api_->query_qpc_frequency();
    if (!frequency.has_value()) {
        return std::unexpected(clock_failure(frequency.error().native_code));
    }
    if (*frequency == 0U) {
        return std::unexpected(clock_failure());
    }

    const auto qpc_before = api_->query_qpc();
    if (!qpc_before.has_value()) {
        return std::unexpected(clock_failure(qpc_before.error().native_code));
    }
    if (*qpc_before == 0U) {
        return std::unexpected(clock_failure());
    }

    const auto source_anchor = api_->current_timestamp();
    const auto qpc_after = api_->query_qpc();
    if (!qpc_after.has_value()) {
        return std::unexpected(clock_failure(qpc_after.error().native_code));
    }
    if (*qpc_after < *qpc_before) {
        return std::unexpected(clock_failure());
    }

    const auto current = api_->current_pad(device_);
    reading_reference current_reading{*api_, current.reading};
    if (current.kind != native_history_read_kind::reading || current_reading.get() == nullptr) {
        const auto cause = current.kind == native_history_read_kind::disconnected
                               ? native_failure_cause::device_disconnected
                               : native_failure_cause::no_current_reading;
        return std::unexpected(native_failure{.cause = cause, .native_code = current.native_code});
    }

    const auto source_timestamp = api_->reading_timestamp(current_reading.get());
    auto state = api_->read_gamepad(current_reading.get());
    if (!state.has_value()) {
        return std::unexpected(state.error());
    }
    if (!is_valid_gamepad_state(*state)) {
        return std::unexpected(native_failure{
            .cause = native_failure_cause::native_failure,
            .native_code = current.native_code,
        });
    }

    const history_clock_anchor anchor{
        .source_timestamp_us = source_anchor,
        .qpc = *qpc_before,
        .qpc_frequency = *frequency,
    };
    const auto mapped_reading = map_history_timestamp(anchor, source_timestamp);
    const auto capture_qpc = api_->query_qpc();
    if (!mapped_reading.has_value()) {
        return std::unexpected(clock_failure());
    }
    if (!capture_qpc.has_value()) {
        return std::unexpected(clock_failure(capture_qpc.error().native_code));
    }
    if (*capture_qpc == 0U) {
        return std::unexpected(clock_failure());
    }

    const auto baseline_qpc = (std::max)(mapped_reading->qpc, *capture_qpc);
    reference_ = std::move(current_reading);
    anchor_ = anchor;
    last_observed_timestamp_us_ = source_anchor;
    last_reading_timestamp_us_ = source_timestamp;
    timeline_floor_qpc_ = baseline_qpc;
    checkpoint_qpc_ = 0U;
    initialized_ = baseline_qpc != 0U;
    if (!initialized_) {
        reference_.reset();
        return std::unexpected(clock_failure());
    }

    return native_history_seed{
        .baseline =
            input_update{
                .state = *state,
                .source_timestamp_us = source_timestamp,
                .observation_qpc = baseline_qpc,
            },
        .qpc_frequency = *frequency,
    };
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

    const auto next_reading = api_->next_pad(device_, reference_.get());
    if (next_reading.kind == native_history_read_kind::no_reading) {
        if (next_reading.reading != nullptr) {
            const reading_reference unexpected_reading{*api_, next_reading.reading};
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
    if (next_reading.kind != native_history_read_kind::reading || next_reading.reading == nullptr) {
        return failure_step(next_reading);
    }

    reading_reference reading{*api_, next_reading.reading};
    const auto source_timestamp = api_->reading_timestamp(reading.get());
    if (source_timestamp < last_reading_timestamp_us_) {
        return clock_discontinuity();
    }

    const auto mapped = map_history_timestamp(anchor_, source_timestamp, timeline_floor_qpc_);
    if (!mapped.has_value()) {
        return clock_discontinuity();
    }

    auto state = api_->read_gamepad(reading.get());
    if (!state.has_value() || !is_valid_gamepad_state(*state)) {
        const auto failure = state.has_value()
                                 ? native_failure{.cause = native_failure_cause::native_failure}
                                 : state.error();
        return native_history_step{
            .kind = native_history_step_kind::provider_failure,
            .native_code = failure.native_code,
        };
    }

    last_reading_timestamp_us_ = source_timestamp;
    timeline_floor_qpc_ = mapped->qpc;
    reference_ = std::move(reading);
    return native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = *state,
                .source_timestamp_us = source_timestamp,
                .observation_qpc = mapped->qpc,
            },
        .checkpoint_qpc = checkpoint_qpc_,
    };
}

} // namespace inputnext::gameinput_detail
