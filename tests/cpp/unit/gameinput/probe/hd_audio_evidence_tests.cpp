#include "audio/audio_service.hpp"
#include "hd_audio_evidence.hpp"
#include "hd_audio_sequence.hpp"
#include "output/playback_test_support.hpp"

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE(
    "gameinput.probe.hd_confirmation_requires_current_accepted_pcm_not_silence_or_consumption",
    "[unit][gameinput][probe][audio]") {
    using namespace inputnext::test::output;
    const auto target = playback_target(1U);
    const auto backend = playback_backend(target, output::backend_id{1U});
    const auto audio = playback_audio(target, output::audio_instance_id{1U});
    const auto playback = playback_identity(
        {.target_value = 1U, .lease_value = 1U, .clip_revision = 1U, .audio = audio});
    const output::body_route_ack route{.backend = backend,
        .publication = output::publication_id{1U},
        .attempt = output::attempt_id{1U},
        .intent = {.identity = playback.lease,
            .revision = 1U,
            .route = output::body_actuator_route::hd}};
    audio_detail::audio_target_progress progress{.target = target,
        .backend = backend,
        .audio = audio,
        .last_admitted_route = route,
        .last_admitted_playback = playback,
        .phase = audio_detail::audio_target_phase::live,
        .transport_open = true,
        .native_stream_started = true,
        .ready = true,
        .source_frames_consumed = 4800U,
        .accepted_silent_buffers = 10U};
    const auto confirmed = [&] {
        return accepted_hd_pcm_progress(backend,
            audio,
            route,
            playback,
            progress,
            0U,
            0U,
            0U,
            4800U);
    };
    CHECK_FALSE(confirmed());
    progress.accepted_pcm_buffers = 1U;
    progress.accepted_pcm_frames = 4799U;
    CHECK_FALSE(confirmed());
    progress.accepted_pcm_frames = 4800U;
    CHECK(confirmed());
    ++progress.last_admitted_playback->clip_revision;
    CHECK_FALSE(confirmed());
    progress.last_admitted_playback = playback;
    progress.counters_saturated = true;
    CHECK_FALSE(confirmed());
    progress.counters_saturated = false;
    progress.ready = false;
    CHECK_FALSE(confirmed());
}

TEST_CASE("gameinput.probe.hd_manual_pulse_is_finite_bounded_and_fades_to_silence",
    "[unit][gameinput][probe][audio]") {
    const auto samples = make_hd_audio_pulse();
    CHECK(samples.size() == hd_audio_pulse_frames());
    CHECK(samples.size() >= output::body_pcm_sample_rate / 2U);
    CHECK(samples.size() <= output::body_pcm_sample_rate);
    CHECK(samples.front() == 0.0F);
    CHECK(samples.back() == 0.0F);
    CHECK(std::ranges::all_of(samples,
        [](const float sample) { return std::isfinite(sample) && std::abs(sample) <= 0.075F; }));
    CHECK(std::ranges::any_of(samples, [](const float sample) { return sample != 0.0F; }));
}

} // namespace
} // namespace inputnext::gameinput_detail
