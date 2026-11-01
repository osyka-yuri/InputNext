#include "gameinput_probe/output/hd_audio/sequence.hpp"

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

constexpr std::array steps{
    hd_audio_step{.name = "hd_pcm_pulse",
        .action = hd_audio_step_action::hd_pulse,
        .preference = output::body_route_preference::prefer_hd,
        .pcm_command = output::body_pcm_command::replace,
        .fallback = {.low_frequency = 0.08F, .high_frequency = 0.08F},
        .instruction =
            "Check the half-second HD pulse, or ordinary vibration if HD is unavailable."},
    hd_audio_step{.name = "classic_fallback",
        .action = hd_audio_step_action::classic_fallback,
        .preference = output::body_route_preference::classic,
        .pcm_command = output::body_pcm_command::stop,
        .fallback = {.low_frequency = 0.12F, .high_frequency = 0.12F},
        .instruction = "Check the brief ordinary vibration."},
    hd_audio_step{.name = "output_off",
        .action = hd_audio_step_action::output_off,
        .preference = output::body_route_preference::classic,
        .pcm_command = output::body_pcm_command::stop,
        .instruction = "Check that the controller is quiet."},
    hd_audio_step{.name = "hd_pcm_recreated",
        .action = hd_audio_step_action::hd_pulse,
        .preference = output::body_route_preference::prefer_hd,
        .pcm_command = output::body_pcm_command::replace,
        .fallback = {.low_frequency = 0.08F, .high_frequency = 0.08F},
        .instruction = "Check the new HD pulse or ordinary vibration after reconnecting."},
};

} // namespace

std::span<const hd_audio_step> hd_audio_steps() noexcept {
    return steps;
}

std::vector<float> make_hd_audio_pulse() {
    std::vector<float> samples(hd_audio_pulse_frames());
    constexpr double frequency = 80.0;
    constexpr double amplitude = 0.075;
    constexpr std::size_t fade_frames = 240U;
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        const auto fade_in = std::min(1.0, static_cast<double>(index) / fade_frames);
        const auto fade_out =
            std::min(1.0, static_cast<double>(samples.size() - 1U - index) / fade_frames);
        const auto phase = 2.0 * std::numbers::pi_v<double> * frequency *
                           static_cast<double>(index) / output::body_pcm_sample_rate;
        samples[index] =
            static_cast<float>(amplitude * std::min(fade_in, fade_out) * std::sin(phase));
    }
    return samples;
}

} // namespace inputnext::gameinput_detail
