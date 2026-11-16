#pragma once

#include "dinput/force_feedback/parameters.hpp"

#include <chrono>

namespace inputnext::dinput::force_feedback {

struct playback_sample final {
    float amplitude{};
    bool finished{};
    std::chrono::steady_clock::time_point next_boundary;
};

// Immutable timing snapshot for one Start call. Evaluation is allocation-free and has no
// dependence on the scheduler's wall clock or output state.
class playback final {
public:
    [[nodiscard]] static bool begin(playback& destination,
        const effect_descriptor& descriptor,
        const effect_parameters& parameters,
        DWORD iterations,
        std::chrono::steady_clock::time_point started_at) noexcept;

    [[nodiscard]] playback_sample evaluate(
        std::chrono::steady_clock::time_point now) const noexcept;

private:
    [[nodiscard]] float force_at(std::uint64_t iteration_elapsed_us) const noexcept;
    [[nodiscard]] float periodic_at(double cycle) const noexcept;
    [[nodiscard]] double envelope_magnitude(double sustain,
        std::chrono::microseconds iteration_elapsed) const noexcept;

    waveform waveform_{};
    effect_parameters parameters_{};
    std::chrono::steady_clock::time_point started_at_;
    std::uint64_t total_duration_us_{};
    bool infinite_duration_{};
    bool infinite_iterations_{};
    bool initialized_{};
};

} // namespace inputnext::dinput::force_feedback
