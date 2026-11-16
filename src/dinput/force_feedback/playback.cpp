#include "playback.hpp"

#include "dinput/force_feedback/parameters.hpp"
#include "dinput/native_types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <variant>

namespace inputnext::dinput::force_feedback {
namespace {

using clock = std::chrono::steady_clock;
inline constexpr std::uint64_t micros_per_tick = 4000U;
inline constexpr std::size_t averaging_samples = 64U;

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
    const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
    const std::uint64_t right) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

[[nodiscard]] std::chrono::steady_clock::time_point deadline_after(
    const std::chrono::steady_clock::time_point start,
    const std::uint64_t microseconds) noexcept {
    const auto maximum =
        std::chrono::duration_cast<std::chrono::microseconds>(clock::time_point::max() - start);
    const auto maximum_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, maximum.count()));
    if (microseconds >= maximum_us) {
        return clock::time_point::max();
    }
    return start + std::chrono::microseconds{static_cast<std::int64_t>(microseconds)};
}

[[nodiscard]] std::uint64_t elapsed_microseconds(const clock::time_point start,
    const clock::time_point now) noexcept {
    if (now <= start) {
        return 0U;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start);
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
}

[[nodiscard]] float normalized_magnitude(const double value) noexcept {
    return static_cast<float>(std::clamp(std::abs(value) / DI_FFNOMINALMAX, 0.0, 1.0));
}

struct iteration_position final {
    std::uint64_t delay{};
    std::uint64_t elapsed{};
    std::uint64_t duration{};
};

[[nodiscard]] std::uint64_t next_iteration_boundary(const iteration_position position) noexcept {
    if (position.duration == 0U || position.duration == std::numeric_limits<std::uint64_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto completed_iterations = position.elapsed / position.duration;
    if (completed_iterations == std::numeric_limits<std::uint64_t>::max()) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto next_elapsed = saturating_multiply(completed_iterations + 1U, position.duration);
    return saturating_add(position.delay, next_elapsed);
}

} // namespace

bool playback::begin(playback& destination,
    const effect_descriptor& descriptor,
    const effect_parameters& parameters,
    const DWORD iterations,
    const clock::time_point started_at) noexcept {
    if (iterations == 0U || !complete(parameters)) {
        return false;
    }

    destination.waveform_ = descriptor.wave;
    destination.parameters_ = parameters;
    destination.started_at_ = started_at;
    destination.infinite_duration_ = parameters.duration == INFINITE;
    destination.infinite_iterations_ = iterations == INFINITE;
    destination.initialized_ = true;
    if (destination.infinite_duration_ || destination.infinite_iterations_) {
        destination.total_duration_us_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        const auto repeated = static_cast<std::uint64_t>(parameters.duration) *
                              static_cast<std::uint64_t>(iterations);
        destination.total_duration_us_ = repeated;
    }
    return true;
}

float playback::periodic_at(const double cycle) const noexcept {
    const auto wrapped = cycle - std::floor(cycle);
    const auto angle = 2.0 * std::numbers::pi * wrapped;
    const auto sine = std::sin(angle);
    switch (waveform_) {
    case waveform::square:
        return sine >= 0.0 ? 1.0F : -1.0F;
    case waveform::sine:
        return static_cast<float>(sine);
    case waveform::triangle:
        return static_cast<float>((2.0 / std::numbers::pi) * std::asin(sine));
    case waveform::sawtooth_up:
        return static_cast<float>((2.0 * wrapped) - 1.0);
    case waveform::sawtooth_down:
        return static_cast<float>(1.0 - (2.0 * wrapped));
    case waveform::constant:
    case waveform::ramp:
        return 0.0F;
    }
    return 0.0F;
}

double playback::envelope_magnitude(const double sustain,
    const std::chrono::microseconds iteration_elapsed) const noexcept {
    if (!parameters_.envelope.has_value()) {
        return sustain;
    }

    const auto& envelope = *parameters_.envelope;
    auto level = sustain;
    const auto elapsed_us = static_cast<std::uint64_t>(iteration_elapsed.count());
    if (envelope.dwAttackTime != 0U && elapsed_us < envelope.dwAttackTime) {
        const auto progress = static_cast<double>(elapsed_us) / envelope.dwAttackTime;
        level = envelope.dwAttackLevel + ((sustain - envelope.dwAttackLevel) * progress);
    }
    if (!infinite_duration_ && parameters_.duration != 0U && envelope.dwFadeTime != 0U) {
        const auto duration = static_cast<std::uint64_t>(parameters_.duration);
        const auto remaining = duration > elapsed_us ? duration - elapsed_us : 0U;
        if (remaining < envelope.dwFadeTime) {
            const auto progress = static_cast<double>(remaining) / envelope.dwFadeTime;
            const auto fade = envelope.dwFadeLevel + ((sustain - envelope.dwFadeLevel) * progress);
            level = fade;
        }
    }
    return std::clamp(level, 0.0, static_cast<double>(DI_FFNOMINALMAX));
}

float playback::force_at(const std::uint64_t iteration_elapsed_us) const noexcept {
    const auto gain = static_cast<double>(parameters_.gain) / DI_FFNOMINALMAX;
    if (const auto* constant = std::get_if<DICONSTANTFORCE>(&parameters_.force);
        constant != nullptr) {
        const auto signed_force = static_cast<double>(constant->lMagnitude);
        const auto magnitude = envelope_magnitude(std::abs(signed_force),
            std::chrono::microseconds{
                static_cast<std::chrono::microseconds::rep>(iteration_elapsed_us)});
        const auto direction = signed_force < 0.0 ? -1.0 : 1.0;
        return normalized_magnitude(direction * magnitude) * static_cast<float>(gain);
    }
    if (const auto* ramp = std::get_if<DIRAMPFORCE>(&parameters_.force); ramp != nullptr) {
        const auto duration = parameters_.duration;
        if (duration == 0U || duration == INFINITE) {
            return 0.0F;
        }
        const auto progress =
            std::clamp(static_cast<double>(iteration_elapsed_us) / duration, 0.0, 1.0);
        const auto value = static_cast<double>(ramp->lStart) +
                           ((static_cast<double>(ramp->lEnd) - ramp->lStart) * progress);
        return normalized_magnitude(value) * static_cast<float>(gain);
    }

    const auto* periodic = std::get_if<DIPERIODIC>(&parameters_.force);
    if (periodic == nullptr || periodic->dwPeriod == 0U) {
        return 0.0F;
    }

    const auto base_cycle = static_cast<double>(periodic->dwPhase) / 36000.0;
    const auto magnitude = envelope_magnitude(periodic->dwMagnitude,
        std::chrono::microseconds{
            static_cast<std::chrono::microseconds::rep>(iteration_elapsed_us)});
    const auto offset = static_cast<double>(periodic->lOffset);
    const auto sample_cycle =
        (static_cast<double>(iteration_elapsed_us) / periodic->dwPeriod) + base_cycle;
    double value{};
    const auto sample_interval =
        parameters_.sample_period == 0U
            ? micros_per_tick
            : std::max<std::uint64_t>(micros_per_tick, parameters_.sample_period);
    // Use mean intensity at the sampling limit so a fixed phase cannot erase the effect.
    if (periodic->dwPeriod <= sample_interval * 2U) {
        for (std::size_t index = 0U; index < averaging_samples; ++index) {
            const auto cycle = base_cycle + (static_cast<double>(index) / averaging_samples);
            value += std::abs(offset + (magnitude * periodic_at(cycle)));
        }
        value /= averaging_samples;
    } else {
        value = std::abs(offset + (magnitude * periodic_at(sample_cycle)));
    }
    return static_cast<float>(std::clamp(value / DI_FFNOMINALMAX, 0.0, 1.0)) *
           static_cast<float>(gain);
}

playback_sample playback::evaluate(const clock::time_point now) const noexcept {
    playback_sample result{};
    if (!initialized_) {
        result.finished = true;
        return result;
    }
    const auto elapsed = elapsed_microseconds(started_at_, now);
    const auto delay = static_cast<std::uint64_t>(parameters_.start_delay);
    const auto full_duration = infinite_duration_ || infinite_iterations_
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : saturating_add(delay, total_duration_us_);
    if (full_duration != std::numeric_limits<std::uint64_t>::max() && elapsed >= full_duration) {
        result.finished = true;
        result.next_boundary = deadline_after(started_at_, full_duration);
        return result;
    }

    if (elapsed < delay) {
        result.next_boundary = deadline_after(started_at_, delay);
        return result;
    }

    const auto effect_elapsed = elapsed - delay;
    const auto duration = static_cast<std::uint64_t>(parameters_.duration);
    auto iteration_elapsed = effect_elapsed;
    if (!infinite_duration_ && duration != 0U) {
        iteration_elapsed %= duration;
    }
    const auto iteration_base = effect_elapsed - iteration_elapsed;
    const auto sample_period =
        parameters_.sample_period == 0U
            ? 0U
            : std::max<std::uint64_t>(micros_per_tick, parameters_.sample_period);
    auto sampled_elapsed = iteration_elapsed;
    if (sample_period != 0U) {
        sampled_elapsed = (sampled_elapsed / sample_period) * sample_period;
    }
    result.amplitude = force_at(sampled_elapsed);
    result.amplitude = std::clamp(result.amplitude, 0.0F, 1.0F);

    auto next_boundary_us = full_duration;
    if (!infinite_duration_) {
        next_boundary_us = std::min(next_boundary_us,
            next_iteration_boundary(iteration_position{
                .delay = delay,
                .elapsed = effect_elapsed,
                .duration = duration,
            }));
    }
    if (sample_period != 0U) {
        const auto sample_index = iteration_elapsed / sample_period;
        const auto next_sample_offset = sample_index == std::numeric_limits<std::uint64_t>::max()
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : saturating_multiply(sample_index + 1U, sample_period);
        const auto next_sample =
            saturating_add(delay, saturating_add(iteration_base, next_sample_offset));
        next_boundary_us = std::min(next_boundary_us, next_sample);
    }
    result.next_boundary = next_boundary_us == std::numeric_limits<std::uint64_t>::max()
                               ? clock::time_point::max()
                               : deadline_after(started_at_, next_boundary_us);
    return result;
}

} // namespace inputnext::dinput::force_feedback
