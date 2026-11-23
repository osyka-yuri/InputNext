#include "bootstrap.hpp"

#include "gameinput/input_profile.hpp"
#include "gameinput/native/gameinput_value_mapping.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/touch_reading.hpp"
#include "gameinput/native/windows_history_cursor.hpp"
#include "gameinput_observation.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "report.hpp"

#include <inputnext/gamepad_state.hpp>

#include <GameInput.h>
#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace inputnext::input_qualification {
namespace {

namespace gameinput = GameInput::v4;
namespace production_input = inputnext::gameinput_detail;

[[nodiscard]] std::optional<std::uint64_t> qpc_now() noexcept {
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == FALSE || counter.QuadPart <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counter.QuadPart);
}

[[nodiscard]] std::uint64_t elapsed_qpc_ticks(const std::optional<std::uint64_t> start,
    const std::optional<std::uint64_t> finish) noexcept {
    if (!start.has_value() || !finish.has_value() || *finish < *start) {
        return 0U;
    }
    return *finish - *start;
}

[[nodiscard]] std::string_view fallback_reason_name(
    const production_input::input_fallback_reason reason) noexcept {
    using production_input::input_fallback_reason;
    switch (reason) {
    case input_fallback_reason::none:
        return "none";
    case input_fallback_reason::unsupported_capability:
        return "unsupported_capability";
    case input_fallback_reason::incoherent_reading:
        return "incoherent_reading";
    case input_fallback_reason::missing_click:
        return "missing_click";
    case input_fallback_reason::invalid_coordinates:
        return "invalid_coordinates";
    case input_fallback_reason::history_lost:
        return "history_lost";
    case input_fallback_reason::incomplete_replay:
        return "incomplete_replay";
    case input_fallback_reason::bootstrap_budget:
        return "bootstrap_budget";
    case input_fallback_reason::epoch_changed:
        return "epoch_changed";
    }
    return "unknown_fallback_reason";
}

[[nodiscard]] std::string_view native_failure_name(
    const production_input::native_failure_cause cause) noexcept {
    using production_input::native_failure_cause;
    switch (cause) {
    case native_failure_cause::load_failed:
        return "native_failure";
    case native_failure_cause::api_unavailable:
        return "api_unavailable";
    case native_failure_cause::initialization_failed:
        return "native_failure";
    case native_failure_cause::registration_failed:
        return "registration_failed";
    case native_failure_cause::no_current_reading:
        return "no_current_reading";
    case native_failure_cause::device_disconnected:
        return "device_disconnected";
    case native_failure_cause::resource_exhausted:
        return "resource_exhausted";
    case native_failure_cause::clock_discontinuity:
        return "clock_discontinuity";
    case native_failure_cause::native_failure:
        return "native_failure";
    }
    return "native_failure";
}

[[nodiscard]] std::string_view touch_source_name(const input::touch_source source) noexcept {
    using input::touch_source;
    switch (source) {
    case touch_source::none:
        return "none";
    case touch_source::left:
        return "left";
    case touch_source::right:
        return "right";
    case touch_source::fallback:
        return "fallback";
    }
    return "none";
}

[[nodiscard]] std::string_view touch_loss_reason_name(
    const input::touch_loss_reason reason) noexcept {
    using input::touch_loss_reason;
    switch (reason) {
    case touch_loss_reason::none:
        return "none";
    case touch_loss_reason::initial_click_held:
        return "initial_click_held";
    case touch_loss_reason::missing_click:
        return "missing_click";
    case touch_loss_reason::incoherent_reading:
        return "incoherent_reading";
    case touch_loss_reason::incomplete_contacts:
        return "incomplete_contacts";
    case touch_loss_reason::invalid_coordinates:
        return "invalid_coordinates";
    case touch_loss_reason::unsupported_capability:
        return "unsupported_capability";
    }
    return "unknown_touch_reason";
}

void measure_cursor_start(bootstrap_attempt_summary& record,
    gameinput::IGameInput* game_input,
    gameinput::IGameInputDevice* device,
    const production_input::native_input_profile& profile,
    const input::touch_snapshot& context,
    const std::uint64_t qpc_frequency) {
    const auto total_start = qpc_now();
    const auto factory_start = total_start;
    auto cursor_result = production_input::create_windows_history_cursor(game_input,
        production_input::native_device_handle{device},
        profile,
        context);
    const auto factory_finish = qpc_now();
    record.factory_measured = factory_start.has_value() && factory_finish.has_value() &&
                              *factory_finish >= *factory_start;
    record.factory_ticks = elapsed_qpc_ticks(factory_start, factory_finish);

    if (!cursor_result.has_value()) {
        const auto& failure = cursor_result.error();
        record.native_code = static_cast<std::int32_t>(failure.native_code);
        record.reason = failure.input_reason != production_input::input_fallback_reason::none
                            ? fallback_reason_name(failure.input_reason)
                            : native_failure_name(failure.cause);
        record.fallback = true;
    } else {
        auto cursor = std::move(*cursor_result);
        const auto start_begin = qpc_now();
        auto seed = cursor->start();
        const auto start_end = qpc_now();
        record.start_measured =
            start_begin.has_value() && start_end.has_value() && *start_end >= *start_begin;
        record.start_ticks = elapsed_qpc_ticks(start_begin, start_end);
        record.transitions = cursor->bootstrap_transitions();
        if (seed.has_value()) {
            record.succeeded = true;
            record.fallback =
                seed->fallback_reason != production_input::input_fallback_reason::none;
            record.reason = fallback_reason_name(seed->fallback_reason);
            record.touch_source = touch_source_name(seed->touch);
            record.touch_reason = touch_loss_reason_name(seed->resolved_touch.reason);
        } else {
            const auto& failure = seed.error();
            record.native_code = static_cast<std::int32_t>(failure.native_code);
            record.reason = failure.input_reason != production_input::input_fallback_reason::none
                                ? fallback_reason_name(failure.input_reason)
                                : native_failure_name(failure.cause);
            record.fallback = true;
        }
        const auto destruction_start = qpc_now();
        cursor.reset();
        const auto destruction_finish = qpc_now();
        record.destruction_measured = destruction_start.has_value() &&
                                      destruction_finish.has_value() &&
                                      *destruction_finish >= *destruction_start;
        record.destruction_ticks = elapsed_qpc_ticks(destruction_start, destruction_finish);
    }

    const auto total_finish = qpc_now();
    record.total_measured =
        total_start.has_value() && total_finish.has_value() && *total_finish >= *total_start;
    record.total_ticks = elapsed_qpc_ticks(total_start, total_finish);
    record.timing_available = qpc_frequency != 0U && record.total_measured;
    if (!record.succeeded && record.reason.empty()) {
        record.reason = "native_failure";
    }
    if (record.reason.empty()) {
        record.reason = "none";
    }
}

} // namespace

bootstrap_measurement::bootstrap_measurement(std::optional<std::uint32_t> click_button)
    : click_button_(click_button) {
    attempts_.reserve(bootstrap_attempt_capacity);
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) != FALSE && frequency.QuadPart > 0) {
        qpc_frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
    }
}

void bootstrap_measurement::configure(const diagnostic::device_report& device) noexcept {
    profile_.reset();
    if (!click_button_.has_value() || device.button_count_truncated ||
        *click_button_ >= device.button_count) {
        return;
    }
    production_input::native_input_profile profile{
        .history = production_input::input_history_path::composite,
        .reason = production_input::input_fallback_reason::none,
        .callback_coherent = true,
        .button_count = device.button_count,
        .click_button = *click_button_,
    };
    if (device.surface_count == 1U && device.surface_info_available.front()) {
        const auto& surface = device.surfaces.front();
        if (surface.type == gameinput::GameInputTouchSurfaceTypeTouchpad &&
            surface.padWidth != 0U && surface.padHeight != 0U && surface.maxContactCount != 0U &&
            surface.maxContactCount <= diagnostic::max_contacts_per_surface) {
            profile.coordinates = production_input::touch_coordinate_source::surface;
            profile.surface_width = surface.padWidth;
            profile.surface_height = surface.padHeight;
        }
    }
    profile_ = profile;
}

void bootstrap_measurement::observe_reading(gameinput::IGameInputReading* reading) noexcept {
    if (!profile_.has_value()) {
        return;
    }
    if (reading == nullptr) {
        static_cast<void>(resolver_.invalidate(input::touch_loss_reason::missing_click));
        return;
    }
    gameinput::GameInputGamepadState native_pad{};
    if (!reading->GetGamepadState(&native_pad) ||
        !inputnext::is_valid_gamepad_state(production_input::map_gamepad_state(native_pad))) {
        static_cast<void>(resolver_.invalidate(input::touch_loss_reason::incoherent_reading));
        return;
    }
    const auto observation = production_input::read_touch_observation(*reading, *profile_);
    const auto resolved = resolver_.observe(observation);
    source_baseline_observed_ =
        source_baseline_observed_ ||
        (resolved.first_accepted_timestamp.has_value() && !resolved.awaiting_release);
}

bootstrap_attempt_result bootstrap_measurement::try_attempt(gameinput::IGameInput* game_input,
    gameinput::IGameInputDevice* device) {
    if (!click_button_.has_value()) {
        return bootstrap_attempt_result::not_requested;
    }
    if (!profile_.has_value()) {
        return bootstrap_attempt_result::invalid_click_index;
    }
    const auto context = resolver_.snapshot();
    if (!source_baseline_observed_ || !context.first_accepted_timestamp.has_value() ||
        context.awaiting_release) {
        return bootstrap_attempt_result::waiting_for_source_baseline;
    }
    if (!context.click_known_pressed) {
        return bootstrap_attempt_result::waiting_for_click_held;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_attempt_) {
        return bootstrap_attempt_result::not_due;
    }
    if (!bootstrap_attempt_capacity_available(attempts_.size())) {
        truncated_ = true;
        return bootstrap_attempt_result::capacity_exhausted;
    }

    bootstrap_attempt_summary record{};
    record.context_baseline_available = source_baseline_observed_ &&
                                        context.first_accepted_timestamp.has_value() &&
                                        !context.awaiting_release;
    record.context_click_pressed = context.click_known_pressed;
    record.context_awaiting_release = context.awaiting_release;
    record.context_epoch = context.epoch;
    record.context_has_first_timestamp = context.first_accepted_timestamp.has_value();
    record.context_first_timestamp = context.first_accepted_timestamp.value_or(0U);

    measure_cursor_start(record, game_input, device, *profile_, context, qpc_frequency_);
    attempts_.push_back(record);
    next_attempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
    return bootstrap_attempt_result::recorded;
}

bootstrap_measurement_summary bootstrap_measurement::summary(const std::string_view status,
    const std::uint32_t observed_button_count) const noexcept {
    return bootstrap_measurement_summary{.requested = click_button_.has_value(),
        .click_index_valid = profile_.has_value(),
        .native_surface_coordinates =
            profile_.has_value() &&
            profile_->coordinates == production_input::touch_coordinate_source::surface,
        .source_baseline_observed = source_baseline_observed_,
        .click_button = click_button_.value_or(0U),
        .observed_button_count = observed_button_count,
        .qpc_frequency = qpc_frequency_,
        .status = status,
        .truncated = truncated_,
        .attempts = std::span<const bootstrap_attempt_summary>{attempts_}};
}

bool bootstrap_measurement::requested() const noexcept {
    return click_button_.has_value();
}

bool bootstrap_measurement::click_index_valid() const noexcept {
    return profile_.has_value();
}

bool bootstrap_measurement::source_baseline_observed() const noexcept {
    return source_baseline_observed_;
}

std::uint32_t bootstrap_measurement::click_button() const noexcept {
    return click_button_.value_or(0U);
}

input::touch_snapshot bootstrap_measurement::touch_context() const noexcept {
    return resolver_.snapshot();
}

std::size_t bootstrap_measurement::attempt_count() const noexcept {
    return attempts_.size();
}

bool bootstrap_measurement::truncated() const noexcept {
    return truncated_;
}

} // namespace inputnext::input_qualification
