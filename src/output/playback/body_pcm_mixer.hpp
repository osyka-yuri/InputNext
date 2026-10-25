#pragma once

#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace inputnext::output::detail {

[[nodiscard]] body_render_result mix_body_pcm_block(body_pcm_view pcm,
    std::uint64_t& cursor,
    std::span<float> quad_output,
    std::size_t requested_frames) noexcept;

} // namespace inputnext::output::detail
