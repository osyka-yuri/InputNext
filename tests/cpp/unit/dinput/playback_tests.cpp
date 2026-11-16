#include "dinput/force_feedback/parameters.hpp"
#include "dinput/force_feedback/playback.hpp"
#include "dinput/native_types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>

using namespace inputnext::dinput::force_feedback;

namespace {

using clock = std::chrono::steady_clock;
using time_point = clock::time_point;

[[nodiscard]] time_point after(time_point start, std::uint64_t microseconds) {
    return start +
           std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(microseconds)};
}

[[nodiscard]] const effect_descriptor* descriptor_for(waveform wave) noexcept {
    for (const auto& descriptor : effects()) {
        if (descriptor.wave == wave) {
            return &descriptor;
        }
    }
    return nullptr;
}

[[nodiscard]] effect_parameters parameters_for(waveform wave, DWORD duration = 10000U) {
    effect_parameters parameters{};
    parameters.valid |= DIEP_DURATION | DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS;
    parameters.duration = duration;
    parameters.axis_count = 2U;
    parameters.direction_count = 2U;
    parameters.axes = {0U, 1U};
    parameters.directions = {1, 0};
    switch (wave) {
    case waveform::constant:
        parameters.force = DICONSTANTFORCE{.lMagnitude = 5000};
        break;
    case waveform::ramp:
        parameters.force = DIRAMPFORCE{.lStart = -5000, .lEnd = 5000};
        break;
    case waveform::square:
    case waveform::sine:
    case waveform::triangle:
    case waveform::sawtooth_up:
    case waveform::sawtooth_down:
        parameters.force =
            DIPERIODIC{.dwMagnitude = 5000U, .lOffset = 0, .dwPhase = 0U, .dwPeriod = 8000U};
        break;
    }
    return parameters;
}

struct playback_attempt final {
    playback value{};
    bool started{};
};

[[nodiscard]] bool cannot_begin(const effect_parameters& parameters,
    waveform wave,
    DWORD iterations,
    time_point start) noexcept {
    playback result{};
    const auto* descriptor = descriptor_for(wave);
    return descriptor != nullptr &&
           !playback::begin(result, *descriptor, parameters, iterations, start);
}

[[nodiscard]] playback_attempt begin_playback(const effect_parameters& parameters,
    waveform wave,
    DWORD iterations,
    time_point start) noexcept {
    playback_attempt result{};
    const auto* descriptor = descriptor_for(wave);
    result.started = descriptor != nullptr &&
                     playback::begin(result.value, *descriptor, parameters, iterations, start);
    return result;
}

} // namespace

TEST_CASE("dinput.playback.honors_delay_and_finite_iteration_boundaries", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::constant, 2000U);
    parameters.start_delay = 1000U;
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::constant, 2U, started);
    REQUIRE(active.started);

    const auto delayed = active.value.evaluate(after(started, 999U));
    CHECK(delayed.amplitude == 0.0F);
    CHECK_FALSE(delayed.finished);
    CHECK(delayed.next_boundary == after(started, 1000U));

    const auto first_start = active.value.evaluate(after(started, 1000U));
    CHECK(first_start.amplitude == Catch::Approx(0.5F));
    CHECK(first_start.next_boundary == after(started, 3000U));

    const auto second_start = active.value.evaluate(after(started, 3000U));
    CHECK_FALSE(second_start.finished);
    CHECK(second_start.amplitude == Catch::Approx(0.5F));
    CHECK(second_start.next_boundary == after(started, 5000U));

    const auto complete = active.value.evaluate(after(started, 5000U));
    CHECK(complete.finished);
    CHECK(complete.amplitude == 0.0F);
    CHECK(complete.next_boundary == after(started, 5000U));
}

TEST_CASE("dinput.playback.restarts_the_envelope_for_each_iteration", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::constant, 10000U);
    parameters.force = DICONSTANTFORCE{.lMagnitude = 5000};
    parameters.envelope = DIENVELOPE{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 0U,
        .dwAttackTime = 1000U,
        .dwFadeLevel = 0U,
        .dwFadeTime = 0U};
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::constant, 2U, started);
    REQUIRE(active.started);

    CHECK(active.value.evaluate(after(started, 0U)).amplitude == Catch::Approx(0.0F));
    CHECK(active.value.evaluate(after(started, 500U)).amplitude == Catch::Approx(0.25F));
    CHECK(active.value.evaluate(after(started, 1000U)).amplitude == Catch::Approx(0.5F));
    CHECK(active.value.evaluate(after(started, 10000U)).amplitude == Catch::Approx(0.0F));
    CHECK(active.value.evaluate(after(started, 10500U)).amplitude == Catch::Approx(0.25F));
    CHECK(active.value.evaluate(after(started, 11000U)).amplitude == Catch::Approx(0.5F));
}

TEST_CASE("dinput.playback.ramp_crosses_zero_and_restarts_from_its_start_force", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::ramp, 10000U);
    parameters.force = DIRAMPFORCE{.lStart = -5000, .lEnd = 5000};
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::ramp, 2U, started);
    REQUIRE(active.started);

    CHECK(active.value.evaluate(after(started, 0U)).amplitude == Catch::Approx(0.5F));
    CHECK(active.value.evaluate(after(started, 2500U)).amplitude == Catch::Approx(0.25F));
    CHECK(active.value.evaluate(after(started, 5000U)).amplitude == Catch::Approx(0.0F));
    CHECK(active.value.evaluate(after(started, 7500U)).amplitude == Catch::Approx(0.25F));
    CHECK(active.value.evaluate(after(started, 10000U)).amplitude == Catch::Approx(0.5F));
    CHECK(active.value.evaluate(after(started, 20000U)).finished);
}

TEST_CASE("dinput.playback.periodic_offset_and_phase_contribute_to_magnitude", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::sine, 20000U);
    parameters.force =
        DIPERIODIC{.dwMagnitude = 6000U, .lOffset = 2000, .dwPhase = 0U, .dwPeriod = 16000U};
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::sine, 1U, started);
    REQUIRE(active.started);

    CHECK(active.value.evaluate(after(started, 0U)).amplitude == Catch::Approx(0.2F));
    CHECK(active.value.evaluate(after(started, 4000U)).amplitude == Catch::Approx(0.8F));
    CHECK(active.value.evaluate(after(started, 8000U)).amplitude == Catch::Approx(0.2F));
    CHECK(active.value.evaluate(after(started, 12000U)).amplitude == Catch::Approx(0.4F));

    parameters.force =
        DIPERIODIC{.dwMagnitude = 6000U, .lOffset = 0, .dwPhase = 9000U, .dwPeriod = 16000U};
    const auto phased = begin_playback(parameters, waveform::sine, 1U, started);
    REQUIRE(phased.started);
    CHECK(phased.value.evaluate(after(started, 0U)).amplitude == Catch::Approx(0.6F));
}

TEST_CASE("dinput.playback.averages_periods_shorter_than_the_scheduler_tick", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::sine, 10000U);
    parameters.force =
        DIPERIODIC{.dwMagnitude = 5000U, .lOffset = 0, .dwPhase = 0U, .dwPeriod = 1000U};
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::sine, 1U, started);
    REQUIRE(active.started);

    const auto first = active.value.evaluate(after(started, 100U));
    const auto second = active.value.evaluate(after(started, 2500U));
    CHECK(first.amplitude == Catch::Approx(0.318F).margin(0.002F));
    CHECK(second.amplitude == Catch::Approx(first.amplitude));
}

TEST_CASE("dinput.playback.samples_on_boundaries_after_each_iteration_reset", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::constant, 10000U);
    parameters.sample_period = 3000U;
    const auto started = time_point{};
    const auto active = begin_playback(parameters, waveform::constant, 2U, started);
    REQUIRE(active.started);

    CHECK(active.value.evaluate(after(started, 1000U)).next_boundary == after(started, 4000U));
    CHECK(active.value.evaluate(after(started, 5000U)).next_boundary == after(started, 8000U));
    CHECK(active.value.evaluate(after(started, 10000U)).next_boundary == after(started, 14000U));

    const auto second_iteration = active.value.evaluate(after(started, 10000U));
    CHECK(second_iteration.next_boundary > after(started, 10000U));
    CHECK(second_iteration.next_boundary == after(started, 14000U));
    CHECK(active.value.evaluate(after(started, 13500U)).next_boundary == after(started, 14000U));
}

TEST_CASE("dinput.playback.averages_a_period_equal_to_the_requested_sample_interval",
    "[unit][dinput]") {
    auto parameters = parameters_for(waveform::sine, 40000U);
    parameters.sample_period = 8000U;
    parameters.force =
        DIPERIODIC{.dwMagnitude = 5000U, .lOffset = 0, .dwPhase = 0U, .dwPeriod = 8000U};
    const auto active = begin_playback(parameters, waveform::sine, 1U, time_point{});
    REQUIRE(active.started);
    for (const auto elapsed : {0U, 8000U, 16000U, 24000U}) {
        CHECK(active.value.evaluate(after(time_point{}, elapsed)).amplitude ==
              Catch::Approx(0.318F).margin(0.002F));
    }
    parameters.sample_period = 0U;
    const auto scheduler_sampled = begin_playback(parameters, waveform::sine, 1U, time_point{});
    REQUIRE(scheduler_sampled.started);
    CHECK(scheduler_sampled.value.evaluate(after(time_point{}, 4000U)).amplitude ==
          Catch::Approx(0.318F).margin(0.002F));
}

TEST_CASE("dinput.playback.fade_can_rise_above_the_sustain_magnitude", "[unit][dinput]") {
    auto parameters = parameters_for(waveform::constant, 10000U);
    parameters.envelope = DIENVELOPE{.dwSize = sizeof(DIENVELOPE),
        .dwAttackLevel = 5000U,
        .dwAttackTime = 0U,
        .dwFadeLevel = 8000U,
        .dwFadeTime = 4000U};
    const auto active = begin_playback(parameters, waveform::constant, 1U, time_point{});
    REQUIRE(active.started);
    CHECK(active.value.evaluate(after(time_point{}, 6000U)).amplitude == Catch::Approx(0.5F));
    CHECK(active.value.evaluate(after(time_point{}, 8000U)).amplitude == Catch::Approx(0.65F));
    CHECK(active.value.evaluate(after(time_point{}, 9999U)).amplitude ==
          Catch::Approx(0.8F).margin(0.001F));
}

TEST_CASE("dinput.playback.supports_meaningful_infinite_playback_and_clamps_output",
    "[unit][dinput]") {
    const auto infinite_duration = parameters_for(waveform::constant, INFINITE);
    const auto started = time_point{};
    const auto infinite = begin_playback(infinite_duration, waveform::constant, 3U, started);
    REQUIRE(infinite.started);
    const auto infinite_sample = infinite.value.evaluate(after(started, 100000000U));
    CHECK_FALSE(infinite_sample.finished);
    CHECK(infinite_sample.next_boundary == time_point::max());
    CHECK(infinite_sample.amplitude == Catch::Approx(0.5F));

    const auto infinite_iterations = parameters_for(waveform::constant, 10000U);
    const auto repeated =
        begin_playback(infinite_iterations, waveform::constant, INFINITE, started);
    REQUIRE(repeated.started);
    CHECK_FALSE(repeated.value.evaluate(after(started, 100000000U)).finished);

    const auto invalid_ramp = parameters_for(waveform::ramp, INFINITE);
    CHECK(cannot_begin(invalid_ramp, waveform::ramp, 1U, started));

    auto saturated = parameters_for(waveform::sine, 10000U);
    saturated.force =
        DIPERIODIC{.dwMagnitude = 10000U, .lOffset = 10000, .dwPhase = 9000U, .dwPeriod = 16000U};
    const auto maxed = begin_playback(saturated, waveform::sine, 1U, started);
    REQUIRE(maxed.started);
    CHECK(maxed.value.evaluate(started).amplitude == Catch::Approx(1.0F));
}
