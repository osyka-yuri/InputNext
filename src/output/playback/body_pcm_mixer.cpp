#include "body_pcm_mixer.hpp"

#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace inputnext::output::detail {
namespace {

[[nodiscard]] float mix_sample(const float sample, const float gain) noexcept {
    const float value = sample * gain;
    if (value < -1.0F) {
        return -1.0F;
    }
    if (value > 1.0F) {
        return 1.0F;
    }
    return value - ((value * value * value) / 6.0F);
}

} // namespace

body_render_result mix_body_pcm_block(const body_pcm_view pcm,
    std::uint64_t& cursor,
    const std::span<float> quad_output,
    const std::size_t requested_frames) noexcept {
    const auto output_frames = quad_output.size() / 4U;
    const auto rendered_frames = std::min(requested_frames, output_frames);
    const auto available_frames =
        cursor < pcm.frame_count ? pcm.frame_count - cursor : std::uint64_t{};
    const auto consumed_frames =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(rendered_frames), available_frames);

    for (std::size_t frame = 0U; frame < rendered_frames; ++frame) {
        const auto output_offset = frame * 4U;
        quad_output[output_offset] = 0.0F;
        quad_output[output_offset + 1U] = 0.0F;

        if (static_cast<std::uint64_t>(frame) >= consumed_frames) {
            quad_output[output_offset + 2U] = 0.0F;
            quad_output[output_offset + 3U] = 0.0F;
            continue;
        }

        const auto source_frame = cursor + static_cast<std::uint64_t>(frame);
        const auto source_offset = static_cast<std::size_t>(source_frame * pcm.channels);
        const float rear_left = mix_sample(pcm.samples[source_offset], pcm.gain);
        if (pcm.channels == 1U) {
            quad_output[output_offset + 2U] = rear_left;
            quad_output[output_offset + 3U] = rear_left;
        } else {
            quad_output[output_offset + 2U] = rear_left;
            quad_output[output_offset + 3U] = mix_sample(pcm.samples[source_offset + 1U], pcm.gain);
        }
    }

    cursor += consumed_frames;
    return body_render_result{.frames_rendered = rendered_frames,
        .source_frames_consumed = static_cast<std::size_t>(consumed_frames),
        .completed = cursor >= pcm.frame_count};
}

} // namespace inputnext::output::detail

namespace inputnext::output {

std::size_t silence_body_block(const std::span<float> quad_output,
    const std::size_t requested_frames) noexcept {
    const auto frames = std::min(requested_frames, quad_output.size() / 4U);
    const auto samples = frames * 4U;
    for (std::size_t index = 0U; index < samples; ++index) {
        quad_output[index] = 0.0F;
    }
    return frames;
}

} // namespace inputnext::output
