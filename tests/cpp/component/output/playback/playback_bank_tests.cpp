#include "output/playback/body_playback_bank.hpp"
#include "output/playback_test_support.hpp"

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using namespace inputnext::output;
using inputnext::output::detail::body_playback_admission_error;
using inputnext::output::detail::body_playback_bank;
using inputnext::output::detail::body_playback_retirement_batch;
using inputnext::test::output::install_voice;
using inputnext::test::output::playback_audio;
using inputnext::test::output::playback_backend;
using inputnext::test::output::playback_identity;
using inputnext::test::output::playback_identity_spec;
using inputnext::test::output::playback_target;
using inputnext::test::output::prepare_mono_pcm;
using inputnext::test::output::take_optional_value;

struct playback_fixture final {
    body_pcm_preparer preparer{};
    std::unique_ptr<body_playback_bank> bank{std::make_unique<body_playback_bank>()};
    std::unique_ptr<body_playback_retirement_batch> retired{
        std::make_unique<body_playback_retirement_batch>()};
};

TEST_CASE("output.playback_bank_serializes_target_across_retired_inflight_voice",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array old_samples{0.25F, 0.5F};
    constexpr std::array new_samples{-0.25F, -0.5F};
    const auto old_identity = playback_identity(
        playback_identity_spec{.target_value = 1U, .lease_value = 10U, .clip_revision = 1U});
    const auto new_identity = playback_identity(
        playback_identity_spec{.target_value = 1U, .lease_value = 10U, .clip_revision = 2U});

    auto old_prepared = prepare_mono_pcm(preparer, old_samples);
    REQUIRE(old_prepared.has_value());
    REQUIRE(install_voice(bank, retired, old_identity, std::move(*old_prepared)));

    std::atomic<bool> old_engine_busy{true};
    auto old_claim = bank.try_claim(old_identity,
        playback_backend(playback_target(1U), backend_id{1U}),
        &old_engine_busy,
        1U);
    REQUIRE(old_claim.has_value());
    REQUIRE(old_claim->has_value());
    auto old_permit = take_optional_value(old_claim.value());

    auto new_prepared = prepare_mono_pcm(preparer, new_samples);
    REQUIRE(new_prepared.has_value());
    REQUIRE(install_voice(bank, retired, new_identity, std::move(*new_prepared), old_identity));

    std::atomic<bool> new_engine_busy{true};
    const auto blocked = bank.try_claim(new_identity,
        playback_backend(playback_target(1U), backend_id{2U}),
        &new_engine_busy,
        1U);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error() == body_playback_admission_error::busy);
    new_engine_busy.store(false, std::memory_order_release);

    std::array<float, 4U> old_output{};
    const auto old_render = std::move(old_permit).render(old_output);
    CHECK(old_render.source_frames_consumed == 1U);
    CHECK_FALSE(old_render.completed);
    CHECK_FALSE(old_engine_busy.load(std::memory_order_acquire));
    CHECK(bank.collect_nonbusy(retired) == 1U);
    REQUIRE(retired.size() == 1U);

    new_engine_busy.store(true, std::memory_order_release);
    auto unblocked = bank.try_claim(new_identity,
        playback_backend(playback_target(1U), backend_id{2U}),
        &new_engine_busy,
        1U);
    REQUIRE(unblocked.has_value());
    REQUIRE(unblocked->has_value());
    std::array<float, 4U> new_output{};
    const auto new_render = take_optional_value(unblocked.value()).render(new_output);
    CHECK(new_render.source_frames_consumed == 1U);
    CHECK(new_output[2U] < 0.0F);
    CHECK_FALSE(new_engine_busy.load(std::memory_order_acquire));
}

TEST_CASE("output.playback_full_bank_preserves_busy_current_and_reuses_idle_current",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array sample{0.4F};
    prepared_body_pcm first_copy{};

    for (std::uint64_t index = 1U; index <= 64U; ++index) {
        auto prepared = prepare_mono_pcm(preparer, sample);
        REQUIRE(prepared.has_value());
        if (index == 1U) {
            first_copy = *prepared;
        }
        const auto identity = playback_identity(playback_identity_spec{.target_value = index,
            .lease_value = index,
            .clip_revision = 1U});
        REQUIRE(install_voice(bank, retired, identity, std::move(*prepared)));
    }
    CHECK(preparer.usage().buffers == 64U);

    const auto old_identity = playback_identity(
        playback_identity_spec{.target_value = 1U, .lease_value = 1U, .clip_revision = 1U});
    std::atomic<bool> engine_busy{true};
    const auto old_slot = bank.find_current(old_identity.lease);
    REQUIRE(old_slot.has_value());
    const auto replacement = playback_identity(
        playback_identity_spec{.target_value = 1U, .lease_value = 1U, .clip_revision = 2U});
    {
        auto claim = bank.try_claim(old_identity,
            playback_backend(playback_target(1U), backend_id{1U}),
            &engine_busy,
            1U);
        REQUIRE(claim.has_value());
        REQUIRE(claim->has_value());
        const auto permit = take_optional_value(claim.value());
        CHECK_FALSE(
            bank.select_install_slot(old_identity.lease.context.target, old_slot).has_value());
        CHECK(first_copy.valid());
        CHECK(bank.find_current(old_identity.lease) == old_slot);
        CHECK(preparer.usage().buffers == 64U);
    }
    // Abandoning the permit clears both busy flags without consuming any source frames.
    CHECK_FALSE(engine_busy.load(std::memory_order_acquire));

    auto replacement_selection =
        bank.select_install_slot(old_identity.lease.context.target, old_slot);
    REQUIRE(replacement_selection.has_value());
    REQUIRE(bank.install(take_optional_value(replacement_selection),
        old_identity,
        replacement,
        std::move(first_copy),
        retired));
    CHECK(bank.find_current(replacement.lease) == old_slot);
    CHECK(preparer.usage().buffers == 64U);

    engine_busy.store(true, std::memory_order_release);
    auto replacement_claim = bank.try_claim(replacement,
        playback_backend(playback_target(1U), backend_id{1U}),
        &engine_busy,
        1U);
    REQUIRE(replacement_claim.has_value());
    REQUIRE(replacement_claim->has_value());
    auto replacement_permit = take_optional_value(replacement_claim.value());
    std::array<float, 4U> output{};
    const auto rendered = std::move(replacement_permit).render(output);
    CHECK(rendered.source_frames_consumed == 1U);
}

TEST_CASE("output.playback_release_without_render_does_not_advance_cursor",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array samples{0.1F, 0.7F};
    const auto identity = playback_identity(
        playback_identity_spec{.target_value = 2U, .lease_value = 20U, .clip_revision = 1U});
    auto prepared = prepare_mono_pcm(preparer, samples);
    REQUIRE(prepared.has_value());
    REQUIRE(install_voice(bank, retired, identity, std::move(*prepared)));

    std::atomic<bool> engine_busy{true};
    {
        auto claim = bank.try_claim(identity,
            playback_backend(playback_target(2U), backend_id{1U}),
            &engine_busy,
            1U);
        REQUIRE(claim.has_value());
        REQUIRE(claim->has_value());
        const auto abandoned = take_optional_value(claim.value());
        CHECK(abandoned.valid());
    }
    CHECK_FALSE(engine_busy.load(std::memory_order_acquire));

    engine_busy.store(true, std::memory_order_release);
    auto claim = bank.try_claim(identity,
        playback_backend(playback_target(2U), backend_id{1U}),
        &engine_busy,
        1U);
    REQUIRE(claim.has_value());
    REQUIRE(claim->has_value());
    std::array<float, 4U> output{};
    const auto rendered = take_optional_value(claim.value()).render(output);
    CHECK(rendered.source_frames_consumed == 1U);
    CHECK(output[2U] == Catch::Approx(0.1F - ((0.1F * 0.1F * 0.1F) / 6.0F)));
}

TEST_CASE("output.playback_retirement_distinguishes_pending_from_started_and_audio_loss",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array samples{0.3F, 0.6F};
    const auto pending_identity = playback_identity(
        playback_identity_spec{.target_value = 3U, .lease_value = 30U, .clip_revision = 1U});
    auto pending = prepare_mono_pcm(preparer, samples);
    REQUIRE(pending.has_value());
    REQUIRE(install_voice(bank, retired, pending_identity, std::move(*pending)));
    CHECK(bank.retire_started_lease(pending_identity.lease) == 0U);

    std::atomic<bool> engine_busy{true};
    {
        const auto claim = bank.try_claim(pending_identity,
            playback_backend(playback_target(3U), backend_id{1U}),
            &engine_busy,
            1U);
        REQUIRE(claim.has_value());
        REQUIRE(claim->has_value());
    }
    CHECK(bank.retire_started_lease(pending_identity.lease) == 1U);
    engine_busy.store(true, std::memory_order_release);
    const auto retired_claim = bank.try_claim(pending_identity,
        playback_backend(playback_target(3U), backend_id{1U}),
        &engine_busy,
        1U);
    REQUIRE(retired_claim.has_value());
    CHECK_FALSE(retired_claim->has_value());
    engine_busy.store(false, std::memory_order_release);
    CHECK(bank.collect_nonbusy(retired) == 1U);

    const auto audio = playback_audio(playback_target(4U), audio_instance_id{44U});
    const auto audio_identity = playback_identity(playback_identity_spec{.target_value = 4U,
        .lease_value = 40U,
        .clip_revision = 1U,
        .audio = audio});
    auto audio_pcm = prepare_mono_pcm(preparer, samples);
    REQUIRE(audio_pcm.has_value());
    REQUIRE(install_voice(bank, retired, audio_identity, std::move(*audio_pcm)));
    CHECK(bank.retire_audio(audio) == 1U);
    CHECK(bank.collect_nonbusy(retired) == 1U);
    REQUIRE(retired.size() == 2U);
}

TEST_CASE("output.playback_backend_retirement_closes_admission_before_drain",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array samples{0.2F, 0.4F};
    const auto identity = playback_identity(
        playback_identity_spec{.target_value = 6U, .lease_value = 60U, .clip_revision = 1U});
    const auto backend = playback_backend(playback_target(6U), backend_id{1U});
    auto prepared = prepare_mono_pcm(preparer, samples);
    REQUIRE(prepared.has_value());
    REQUIRE(install_voice(bank, retired, identity, std::move(*prepared)));

    std::atomic<bool> engine_busy{true};
    auto claim = bank.try_claim(identity, backend, &engine_busy, 1U);
    REQUIRE(claim.has_value());
    REQUIRE(claim->has_value());
    auto permit = take_optional_value(claim.value());

    CHECK(bank.retire_backend(backend) == 1U);
    CHECK(bank.collect_nonbusy(retired) == 0U);
    std::atomic<bool> retry_engine_busy{true};
    const auto retry = bank.try_claim(identity, backend, &retry_engine_busy, 1U);
    REQUIRE_FALSE(retry.has_value());
    CHECK(retry.error() == body_playback_admission_error::busy);
    retry_engine_busy.store(false, std::memory_order_release);

    std::array<float, 4U> output{};
    CHECK(std::move(permit).render(output).source_frames_consumed == 1U);
    CHECK(bank.collect_nonbusy(retired) == 1U);
    CHECK_FALSE(engine_busy.load(std::memory_order_acquire));

    retry_engine_busy.store(true, std::memory_order_release);
    const auto closed = bank.try_claim(identity, backend, &retry_engine_busy, 1U);
    REQUIRE(closed.has_value());
    CHECK_FALSE(closed->has_value());
    retry_engine_busy.store(false, std::memory_order_release);
}

TEST_CASE("output.playback_stop_retires_pending_voice_without_a_silent_permit",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    auto& retired = *fixture.retired;
    constexpr std::array samples{0.2F, 0.3F};
    const auto identity = playback_identity(
        playback_identity_spec{.target_value = 7U, .lease_value = 70U, .clip_revision = 1U});
    auto prepared = prepare_mono_pcm(preparer, samples);
    REQUIRE(prepared.has_value());
    REQUIRE(install_voice(bank, retired, identity, std::move(*prepared)));

    CHECK(bank.stop(identity.lease));
    std::atomic<bool> engine_busy{true};
    const auto claim = bank.try_claim(identity,
        playback_backend(playback_target(7U), backend_id{1U}),
        &engine_busy,
        2U);
    REQUIRE(claim.has_value());
    CHECK_FALSE(claim->has_value());
    engine_busy.store(false, std::memory_order_release);
    CHECK(bank.collect_nonbusy(retired) == 1U);
}

TEST_CASE("output.playback_completion_collects_owner_only_after_release",
    "[component][output][playback]") {
    playback_fixture fixture{};
    auto& preparer = fixture.preparer;
    auto& bank = *fixture.bank;
    constexpr std::array samples{0.2F};
    const auto identity = playback_identity(
        playback_identity_spec{.target_value = 5U, .lease_value = 50U, .clip_revision = 1U});
    auto prepared = prepare_mono_pcm(preparer, samples);
    REQUIRE(prepared.has_value());
    REQUIRE(install_voice(bank, *fixture.retired, identity, std::move(*prepared)));

    std::atomic<bool> engine_busy{true};
    auto claim = bank.try_claim(identity,
        playback_backend(playback_target(5U), backend_id{1U}),
        &engine_busy,
        3U);
    REQUIRE(claim.has_value());
    REQUIRE(claim->has_value());
    std::array<float, 12U> output{};
    const auto rendered = take_optional_value(claim.value()).render(output);
    CHECK(rendered.frames_rendered == 3U);
    CHECK(rendered.source_frames_consumed == 1U);
    CHECK(rendered.completed);
    CHECK_FALSE(engine_busy.load(std::memory_order_acquire));
    CHECK(output[6U] == 0.0F);
    CHECK(output[11U] == 0.0F);

    {
        const auto completed = std::make_unique<body_playback_retirement_batch>();
        CHECK(bank.collect_nonbusy(*completed) == 1U);
        REQUIRE(completed->size() == 1U);
        CHECK(preparer.usage().buffers == 1U);
    }
    CHECK(preparer.usage().buffers == 0U);
}

} // namespace
