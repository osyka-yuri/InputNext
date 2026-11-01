#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace inputnext::gameinput_detail {

enum class hd_audio_step_action : std::uint8_t {
    hd_pulse,
    classic_fallback,
    output_off,
};

struct hd_audio_step final {
    std::string_view name;
    hd_audio_step_action action{};
    output::body_route_preference preference{output::body_route_preference::prefer_hd};
    output::body_pcm_command pcm_command{output::body_pcm_command::keep};
    output::rumble_value fallback{};
    std::string_view instruction;
};

[[nodiscard]] std::span<const hd_audio_step> hd_audio_steps() noexcept;

[[nodiscard]] constexpr std::size_t hd_audio_pulse_frames() noexcept {
    return 24'000U;
}

[[nodiscard]] constexpr float hd_audio_pulse_gain() noexcept {
    return 0.5F;
}

[[nodiscard]] std::vector<float> make_hd_audio_pulse();

} // namespace inputnext::gameinput_detail
