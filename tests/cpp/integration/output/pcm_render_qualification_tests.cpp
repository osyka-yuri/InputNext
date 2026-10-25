#include "body_playback_fixture.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/playback_test_support.hpp"
#include "values/required_value.hpp"

#include "output/execution/engine.hpp"
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::required_value;
using inputnext::test::output::body_playback_fixture;
using inputnext::test::output::take_optional_value;

struct accepted_playback final {
    body_playback_identity identity;
    body_route_ack acknowledgement;
};

[[nodiscard]] accepted_playback start_playback(body_playback_fixture& fixture) {
    constexpr std::array samples{0.2F, 0.4F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    const auto installed = fixture.replace(std::move(*prepared));
    REQUIRE(installed.has_value());
    REQUIRE(installed->playback.has_value());
    REQUIRE(fixture.publish_and_drive());
    const auto acknowledgement = fixture.acknowledgement();
    REQUIRE(acknowledgement.has_value());
    return {.identity = required_value(installed->playback),
        .acknowledgement = required_value(acknowledgement)};
}

TEST_CASE("output.pcm_render_admission_rejects_foreign_proofs_and_invalid_block_sizes",
    "[integration][output][body-pcm]") {
    body_playback_fixture fixture;
    const auto accepted = start_playback(fixture);
    auto& engine = fixture.engine->engine();
    for (const auto frames : {std::size_t{0U}, std::numeric_limits<std::size_t>::max()}) {
        const auto invalid =
            engine.admit_body_render(accepted.acknowledgement, accepted.identity, frames);
        CHECK((!invalid.has_value() && invalid.error() == execution_error::invalid_argument));
    }

    auto forged_ack = accepted.acknowledgement;
    ++forged_ack.attempt.value;
    const auto rejected_proof = engine.admit_body_render(forged_ack, accepted.identity, 1U);
    REQUIRE(rejected_proof.has_value());
    CHECK_FALSE(rejected_proof->has_value());

    auto foreign = accepted.identity;
    ++foreign.lease.context.id.value;
    const auto rejected_context = engine.admit_body_render(accepted.acknowledgement, foreign, 1U);
    REQUIRE(rejected_context.has_value());
    CHECK_FALSE(rejected_context->has_value());
    foreign = accepted.identity;
    ++foreign.lease.audio.id.value;
    const auto rejected_audio = engine.admit_body_render(accepted.acknowledgement, foreign, 1U);
    REQUIRE(rejected_audio.has_value());
    CHECK_FALSE(rejected_audio->has_value());
    foreign = accepted.identity;
    ++foreign.clip_revision;
    const auto rejected_clip = engine.admit_body_render(accepted.acknowledgement, foreign, 1U);
    REQUIRE(rejected_clip.has_value());
    CHECK_FALSE(rejected_clip->has_value());
    auto exact = engine.admit_body_render(accepted.acknowledgement, accepted.identity, 1U);
    REQUIRE(exact.has_value());
    REQUIRE(exact->has_value());
}

TEST_CASE("output.pcm_render_admission_rechecks_expiry_before_claiming_samples",
    "[integration][output][body-pcm]") {
    using clock = std::chrono::steady_clock;
    std::atomic<std::int64_t> ticks{clock::now().time_since_epoch().count()};
    body_playback_fixture fixture;
    ticks.store(clock::now().time_since_epoch().count(), std::memory_order_release);
    inputnext::output::detail::authority_test_access::use_clock(fixture.authority.owner, ticks);
    const auto accepted = start_playback(fixture);
    ticks.fetch_add(std::chrono::duration_cast<clock::duration>(6s).count(),
        std::memory_order_release);
    const auto expired =
        fixture.engine->engine().admit_body_render(accepted.acknowledgement, accepted.identity, 1U);
    REQUIRE(expired.has_value());
    CHECK_FALSE(expired->has_value());
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
}

TEST_CASE("output.pcm_pending_clip_can_start_once_when_its_original_route_becomes_ready",
    "[integration][output][body-pcm]") {
    body_playback_fixture fixture;
    constexpr std::array samples{0.3F};
    auto prepared = fixture.prepare(samples);
    REQUIRE(prepared.has_value());
    auto request = fixture.request(1U);
    request.preference = body_route_preference::classic;
    const auto pending = fixture.authority.owner.submit_body(fixture.context,
        request,
        body_playback_update{.command = body_pcm_command::replace,
            .replacement = std::move(*prepared)});
    REQUIRE(pending.has_value());
    fixture.body = pending->body;
    const auto identity = required_value(pending->playback);
    const auto kept = fixture.keep(2U);
    REQUIRE(kept.has_value());
    CHECK(kept->playback == identity);
    REQUIRE(fixture.publish_and_drive());
    auto admitted = fixture.admit(identity);
    REQUIRE(admitted.has_value());
    REQUIRE(admitted->has_value());
    std::array<float, 4U> output{};
    CHECK(take_optional_value(admitted.value()).render(output).completed);
    const auto kept_after_completion = fixture.keep(3U);
    REQUIRE(kept_after_completion.has_value());
    const auto replay = fixture.admit(identity);
    REQUIRE(replay.has_value());
    CHECK_FALSE(replay->has_value());
}

TEST_CASE("output.pcm_started_clip_cannot_resume_after_an_explicit_classic_transition",
    "[integration][output][body-pcm]") {
    body_playback_fixture fixture;
    const auto accepted = start_playback(fixture);
    {
        auto admitted = fixture.admit(accepted.identity);
        REQUIRE(admitted.has_value());
        REQUIRE(admitted->has_value());
    }
    auto classic = fixture.request(2U);
    classic.preference = body_route_preference::classic;
    const auto suspended = fixture.authority.owner.submit_body(fixture.context, classic);
    REQUIRE(suspended.has_value());
    fixture.body = *suspended;
    REQUIRE(fixture.keep(3U).has_value());
    REQUIRE(fixture.publish_and_drive());
    const auto replay = fixture.admit(accepted.identity);
    REQUIRE(replay.has_value());
    CHECK_FALSE(replay->has_value());
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
}

TEST_CASE("output.pcm_focus_revocation_closes_admission_while_retaining_an_admitted_block",
    "[integration][output][body-pcm][concurrency]") {
    body_playback_fixture fixture;
    const auto accepted = start_playback(fixture);
    auto admitted = fixture.admit(accepted.identity);
    REQUIRE(admitted.has_value());
    auto permit = take_optional_value(admitted.value());

    fixture.authority.focus_observer.stop();
    const auto rejected =
        fixture.engine->engine().admit_body_render(accepted.acknowledgement, accepted.identity, 1U);
    REQUIRE(rejected.has_value());
    CHECK_FALSE(rejected->has_value());
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);

    std::array<float, 4U> output{};
    CHECK(std::move(permit).render(output).source_frames_consumed == 1U);
    CHECK(output[2U] > 0.0F);
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
}

TEST_CASE("output.pcm_connection_replacement_preserves_admitted_samples_and_new_input_progress",
    "[integration][output][body-pcm][concurrency]") {
    body_playback_fixture fixture;
    const auto accepted = start_playback(fixture);
    auto admitted = fixture.admit(accepted.identity);
    REQUIRE(admitted.has_value());
    auto permit = take_optional_value(admitted.value());

    std::unique_ptr<inputnext::source_lifetime> source =
        std::make_unique<inputnext::test::output::registry_source>();
    const auto replacement =
        fixture.authority.registry.replace(fixture.authority.connection.target, source);
    REQUIRE(replacement.has_value());
    CHECK(replacement->target != fixture.authority.connection.target);
    const auto rejected =
        fixture.engine->engine().admit_body_render(accepted.acknowledgement, accepted.identity, 1U);
    REQUIRE(rejected.has_value());
    CHECK_FALSE(rejected->has_value());
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 1U);

    const inputnext::input_update update{.state = inputnext::gamepad_state{.left_trigger = 0.75F},
        .source_timestamp_us = 400U,
        .observation_qpc = 500U};
    REQUIRE(replacement->producer.publish(update).has_value());
    const auto latest = replacement->reader.read();
    REQUIRE(latest.has_value());
    CHECK(latest->sample.state == update.state);

    std::array<float, 4U> output{};
    CHECK(std::move(permit).render(output).source_frames_consumed == 1U);
    CHECK(output[2U] > 0.0F);
    fixture.authority.owner.collect_body_pcm();
    CHECK(fixture.authority.owner.pcm_preparer().usage().buffers == 0U);
}

} // namespace
