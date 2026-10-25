#include "output/playback/body_pcm_mixer.hpp"

#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

using inputnext::output::body_pcm_view;
using inputnext::output::silence_body_block;
using inputnext::output::detail::mix_body_pcm_block;

static_assert(!std::is_copy_constructible_v<inputnext::output::body_render_permit>);
static_assert(std::is_nothrow_move_constructible_v<inputnext::output::body_render_permit>);

TEST_CASE("output.body_mixer_duplicates_mono_and_soft_clips_normalized_endpoints",
    "[unit][output][playback]") {
    constexpr std::array samples{-1.0F, -0.5F, 0.0F, 0.5F, 1.0F};
    const body_pcm_view pcm{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = 48000U,
        .gain = 1.0F};
    std::array<float, samples.size() * 4U> output{};
    std::uint64_t cursor{};

    const auto rendered = mix_body_pcm_block(pcm, cursor, output, samples.size());

    CHECK(rendered.frames_rendered == samples.size());
    CHECK(rendered.source_frames_consumed == samples.size());
    CHECK(rendered.completed);
    CHECK(cursor == samples.size());
    for (std::size_t frame = 0U; frame < samples.size(); ++frame) {
        const auto frame_offset = frame * 4U;
        CHECK(output[frame_offset] == 0.0F);
        CHECK(output[frame_offset + 1U] == 0.0F);
        CHECK(output[frame_offset + 2U] == output[frame_offset + 3U]);
    }
    CHECK(output[2U] == Catch::Approx(-5.0F / 6.0F));
    CHECK(output[6U] == Catch::Approx(-0.47916666F));
    CHECK(output[14U] == Catch::Approx(0.47916666F));
    CHECK(output[18U] == Catch::Approx(5.0F / 6.0F));
}

TEST_CASE("output.body_mixer_gains_stereo_and_saturates_outside_unit_range",
    "[unit][output][playback]") {
    constexpr std::array samples{-0.4F, 0.2F, 2.0F, -2.0F};
    const body_pcm_view pcm{.samples = samples,
        .frame_count = 2U,
        .channels = 2U,
        .sample_rate = 48000U,
        .gain = 1.5F};
    std::array<float, 8U> output{};
    std::uint64_t cursor{};

    const auto rendered = mix_body_pcm_block(pcm, cursor, output, 2U);

    CHECK(rendered.frames_rendered == 2U);
    CHECK(rendered.source_frames_consumed == 2U);
    CHECK(rendered.completed);
    CHECK(output[2U] == Catch::Approx(-0.564F));
    CHECK(output[3U] == Catch::Approx(0.2955F));
    CHECK(output[6U] == 1.0F);
    CHECK(output[7U] == -1.0F);
}

TEST_CASE("output.body_mixer_zero_pads_clip_end_and_preserves_partial_quad",
    "[unit][output][playback]") {
    constexpr std::array samples{0.25F};
    const body_pcm_view pcm{.samples = samples,
        .frame_count = 1U,
        .channels = 1U,
        .sample_rate = 48000U,
        .gain = 1.0F};
    std::array<float, 14U> output{};
    output.fill(9.0F);
    std::uint64_t cursor{};

    const auto rendered = mix_body_pcm_block(pcm, cursor, output, 3U);

    CHECK(rendered.frames_rendered == 3U);
    CHECK(rendered.source_frames_consumed == 1U);
    CHECK(rendered.completed);
    CHECK(cursor == 1U);
    CHECK(output[2U] == Catch::Approx(0.25F - ((0.25F * 0.25F * 0.25F) / 6.0F)));
    CHECK(output[3U] == output[2U]);
    for (std::size_t index = 4U; index < 12U; ++index) {
        CHECK(output[index] == 0.0F);
    }
    CHECK(output[12U] == 9.0F);
    CHECK(output[13U] == 9.0F);
}

TEST_CASE("output.body_mixer_short_buffer_limits_work_to_complete_quads",
    "[unit][output][playback]") {
    constexpr std::array samples{0.1F, 0.2F};
    const body_pcm_view pcm{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = 48000U,
        .gain = 1.0F};
    std::array<float, 5U> output{};
    output.fill(7.0F);
    std::uint64_t cursor{};

    const auto rendered = mix_body_pcm_block(pcm, cursor, output, 10U);

    CHECK(rendered.frames_rendered == 1U);
    CHECK(rendered.source_frames_consumed == 1U);
    CHECK_FALSE(rendered.completed);
    CHECK(cursor == 1U);
    CHECK(output[4U] == 7.0F);
}

TEST_CASE("output.body_silence_helper_clears_only_bounded_complete_quads",
    "[unit][output][playback]") {
    std::array<float, 11U> output{};
    output.fill(3.0F);

    CHECK(silence_body_block(output, 9U) == 2U);
    for (std::size_t index = 0U; index < 8U; ++index) {
        CHECK(output[index] == 0.0F);
    }
    CHECK(output[8U] == 3.0F);
    CHECK(output[9U] == 3.0F);
    CHECK(output[10U] == 3.0F);
}

} // namespace
