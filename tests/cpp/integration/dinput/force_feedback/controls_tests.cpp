#include "dinput/force_feedback/parameters.hpp"
#include "dinput/profile.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/window_acquisition.hpp"
#include "support.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::dinput::force_feedback;
using namespace std::chrono_literals;
using inputnext::dinput::feedback_test_support::constant;
using inputnext::dinput::feedback_test_support::feedback_fixture;

TEST_CASE("dinput.feedback.staged_updates_and_device_gain_preserve_playback_contracts",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto first = fixture.create(&fixture);
    const auto second = fixture.create(&fixture);
    auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    DICONSTANTFORCE force{1000};
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.cbTypeSpecificParams = sizeof(force);
    update.lpvTypeSpecificParams = &force;
    REQUIRE(fixture.feedback->set(first,
                &update,
                DIEP_TYPESPECIFICPARAMS | DIEP_NODOWNLOAD,
                nullptr,
                acquired) == DI_DOWNLOADSKIPPED);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.1F));
    REQUIRE(fixture.feedback->stop(first) == DI_OK);
    acquired.gain = 5000U;
    REQUIRE(fixture.feedback->start(second, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.feedback->set_gain(&fixture, 5000U) == DI_OK);
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.45F));
    const auto third = fixture.create(&fixture);
    REQUIRE(fixture.feedback->start(third, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.85F));
}

TEST_CASE("dinput.feedback.failed_solo_start_preserves_another_playing_effect",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto playing = fixture.create(&fixture);
    const auto undownloaded = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(playing, {.iterations = 1U, .flags = 0U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(FAILED(fixture.feedback->start(undownloaded,
        {.iterations = 1U, .flags = DIES_SOLO | DIES_NODOWNLOAD},
        acquired)));
    DWORD status{};
    REQUIRE(fixture.feedback->status(playing, &status) == DI_OK);
    CHECK((status & DIEGES_PLAYING) != 0U);
    const auto state = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(state.has_value());
    REQUIRE(state->rumble.has_value());
    CHECK(state->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .value.low_frequency == Catch::Approx(0.8F));
}

TEST_CASE("dinput.feedback.solo_rejection_preserves_playback_and_its_publication",
    "[integration][dinput][force_feedback][parameters][cleanup]") {
    feedback_fixture fixture;
    const auto acquired = fixture.acquisition();
    const auto first = fixture.create(&fixture);
    auto parameters = constant(2000);
    bool clear_failure{};
    SECTION("submit failure") {
    }
    SECTION("clear failure before delayed playback") {
        parameters.start_delay = 100000U;
        clear_failure = true;
    }
    std::uint64_t second{};
    REQUIRE(
        fixture.feedback->create(&fixture, *find_effect(GUID_ConstantForce), parameters, second) ==
        DI_OK);
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.pause_next_query());
    using access = output::detail::authority_test_access;
    auto& authority = fixture.trusted.public_authority();
    access::fail_internal_cleanup(authority, output::error::resources, output::error::resources);
    access::fail_internal_mutation(authority,
        clear_failure ? std::nullopt : std::optional{output::error::resources},
        clear_failure ? std::optional{output::error::resources} : std::nullopt);
    const auto result =
        fixture.feedback->start(second, {.iterations = 1U, .flags = DIES_SOLO}, acquired);
    access::fail_internal_mutation(authority, std::nullopt, std::nullopt);
    access::fail_internal_cleanup(authority, std::nullopt, std::nullopt);
    CHECK(FAILED(result));
    DWORD state{};
    REQUIRE(fixture.feedback->status(first, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) != 0U);
    REQUIRE(fixture.feedback->status(second, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) == 0U);
    const auto current = authority.read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    REQUIRE(current->rumble.has_value());
    CHECK(current->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
              .value.low_frequency == Catch::Approx(0.8F));
    CHECK(fixture.feedback->start(second, {.iterations = 1U, .flags = DIES_NODOWNLOAD}, acquired) ==
          DIERR_NOTDOWNLOADED);
    fixture.release_query.store(true, std::memory_order_release);
}

TEST_CASE("dinput.feedback.solo_start_reuses_its_context_without_requiring_close",
    "[integration][dinput][force_feedback][cleanup]") {
    feedback_fixture fixture;
    const auto first = fixture.create(&fixture);
    const auto second = fixture.create(&fixture, 2000);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(first, {.iterations = 1U}, acquired) == DI_OK);
    using access = output::detail::authority_test_access;
    auto& authority = fixture.trusted.public_authority();
    access::fail_internal_cleanup(authority, output::error::resources, output::error::resources);
    const auto result =
        fixture.feedback->start(second, {.iterations = 1U, .flags = DIES_SOLO}, acquired);
    access::fail_internal_cleanup(authority, std::nullopt, std::nullopt);
    REQUIRE(result == DI_OK);
    DWORD state{};
    REQUIRE(fixture.feedback->status(first, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) == 0U);
    REQUIRE(fixture.feedback->status(second, &state) == DI_OK);
    CHECK((state & DIEGES_PLAYING) != 0U);
    REQUIRE(fixture.wait_for_amplitude(0.2F));
}

TEST_CASE("dinput.feedback.rejected_dynamic_download_preserves_the_playing_version",
    "[integration][dinput][force_feedback][parameters][cleanup]") {
    feedback_fixture fixture;
    const auto acquired = fixture.acquisition();
    const auto id = fixture.create(&fixture);
    REQUIRE(fixture.feedback->start(id, {.iterations = INFINITE}, acquired) == DI_OK);
    REQUIRE(fixture.pause_next_query());
    const auto before = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(before.has_value());
    REQUIRE(before->rumble.has_value());
    const auto old_identity =
        before->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
            .provenance.identity;
    bool explicit_download{};
    DWORD gain{2500U};
    SECTION("SetParameters submit rejection") {
    }
    SECTION("SetParameters clear rejection") {
        gain = 0U;
    }
    SECTION("Download submit rejection") {
        explicit_download = true;
    }
    SECTION("Download clear rejection") {
        explicit_download = true;
        gain = 0U;
    }
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwGain = gain;
    if (explicit_download) {
        REQUIRE(
            fixture.feedback->set(id, &update, DIEP_GAIN | DIEP_NODOWNLOAD, nullptr, acquired) ==
            DI_DOWNLOADSKIPPED);
    }
    using access = output::detail::authority_test_access;
    auto& authority = fixture.trusted.public_authority();
    access::fail_internal_mutation(authority, output::error::resources, output::error::resources);
    const auto result =
        explicit_download
            ? fixture.feedback->download(id, acquired)
            : fixture.feedback->set(id, &update, DIEP_GAIN | DIEP_NORESTART, nullptr, acquired);
    access::fail_internal_mutation(authority, std::nullopt, std::nullopt);
    CHECK(FAILED(result));
    const auto current = authority.read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    REQUIRE(current->rumble.has_value());
    const auto rumble = current->rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(rumble.provenance.identity == old_identity);
    CHECK(rumble.value.low_frequency == Catch::Approx(0.8F));
    REQUIRE(
        fixture.feedback->start(id, {.iterations = INFINITE, .flags = DIES_NODOWNLOAD}, acquired) ==
        DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->download(id, acquired) == DI_OK);
    const auto updated = authority.read_snapshot(fixture.target);
    REQUIRE(updated.has_value());
    if (gain == 0U) {
        CHECK_FALSE(updated->rumble.has_value());
    } else {
        REQUIRE(updated->rumble.has_value());
        CHECK(updated->rumble.value_or(output::channel_snapshot<output::rumble_value>{})
                  .value.low_frequency == Catch::Approx(0.2F));
    }
    fixture.release_query.store(true, std::memory_order_release);
}

TEST_CASE("dinput.feedback.conflicting_parameter_controls_preserve_staged_values",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwGain = 1000U;
    CHECK(fixture.feedback->set(id,
              &update,
              DIEP_GAIN | DIEP_NODOWNLOAD | DIEP_START,
              nullptr,
              fixture.acquisition()) == DIERR_INVALIDPARAM);
    DIEFFECT result{};
    result.dwSize = sizeof(result);
    REQUIRE(fixture.feedback->get(id, &result, DIEP_GAIN, nullptr) == DI_OK);
    CHECK(result.dwGain == DI_FFNOMINALMAX);
}

TEST_CASE("dinput.feedback.download_updates_active_playback_and_zero_flags_download_staged_values",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    DICONSTANTFORCE force{1000};
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.cbTypeSpecificParams = sizeof(force);
    update.lpvTypeSpecificParams = &force;
    REQUIRE(fixture.feedback->set(id,
                &update,
                DIEP_TYPESPECIFICPARAMS | DIEP_NODOWNLOAD,
                nullptr,
                acquired) == DI_DOWNLOADSKIPPED);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->download(id, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.1F));
    force.lMagnitude = 3000;
    REQUIRE(fixture.feedback->set(id,
                &update,
                DIEP_TYPESPECIFICPARAMS | DIEP_NODOWNLOAD,
                nullptr,
                acquired) == DI_DOWNLOADSKIPPED);
    REQUIRE(fixture.feedback->set(id, nullptr, 0U, nullptr, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.3F));
    const auto offline = fixture.create(&fixture);
    CHECK(fixture.feedback->set(offline, nullptr, 0U, nullptr, {}) == DI_DOWNLOADSKIPPED);
}

TEST_CASE("dinput.feedback.repeated_download_distinguishes_staged_changes_and_acquisition",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->download(id, acquired) == DI_OK);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
    fixture.available.store(false, std::memory_order_release);
    CHECK(fixture.feedback->download(id, acquired) == DIERR_UNSUPPORTED);
    fixture.available.store(true, std::memory_order_release);
    CHECK(fixture.feedback->download(id, acquired) == DI_OK);
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwGain = 5000U;
    REQUIRE(fixture.feedback->set(id, &update, DIEP_GAIN | DIEP_NODOWNLOAD, nullptr, acquired) ==
            DI_DOWNLOADSKIPPED);
    REQUIRE(fixture.feedback->download(id, acquired) == DI_OK);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
    REQUIRE(fixture.wait_for_amplitude(0.4F));
    acquired.token->revoke();
    REQUIRE(fixture.feedback->download(id, fixture.acquisition()) == DI_OK);
}

TEST_CASE("dinput.feedback.ramp_absent_envelope_does_not_require_restart_or_download",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    auto parameters = constant();
    parameters.force = DIRAMPFORCE{.lStart = 8000, .lEnd = 8000};
    parameters.duration = 1000000U;
    std::uint64_t id{};
    REQUIRE(
        fixture.feedback->create(&fixture, *find_effect(GUID_RampForce), parameters, id) == DI_OK);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = INFINITE}, acquired) == DI_OK);
    DWORD controls{};
    SECTION("automatic download") {
    }
    SECTION("no restart") {
        controls = DIEP_NORESTART;
    }
    SECTION("staging only") {
        controls = DIEP_NODOWNLOAD;
    }
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    const auto expected = controls == DIEP_NODOWNLOAD ? DI_DOWNLOADSKIPPED : DI_OK;
    REQUIRE(fixture.feedback->set(id, &update, DIEP_ENVELOPE | controls, nullptr, acquired) ==
            expected);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
    CHECK(fixture.feedback->set(id, nullptr, DIEP_NORESTART, nullptr, acquired) == DI_OK);
    DWORD status{};
    REQUIRE(fixture.feedback->status(id, &status) == DI_OK);
    CHECK((status & DIEGES_PLAYING) != 0U);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
}

TEST_CASE("dinput.feedback.axes_can_change_while_stopped_or_through_a_playback_restart",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto acquired = fixture.acquisition();
    bool playing{};
    bool no_restart{};
    SECTION("stopped") {
    }
    SECTION("playing") {
        playing = true;
    }
    SECTION("playing with no restart") {
        playing = true;
        no_restart = true;
    }
    for (const auto counts : std::array{std::pair{1U, 1U}, std::pair{2U, 1U}, std::pair{1U, 2U}}) {
        auto initial = constant();
        initial.axis_count = counts.first;
        initial.direction_count = counts.first;
        initial.axes = {0U, 1U};
        initial.directions = {1, 1};
        std::uint64_t id{};
        REQUIRE(fixture.feedback->create(&fixture, *find_effect(GUID_ConstantForce), initial, id) ==
                DI_OK);
        if (playing) {
            REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
        }
        std::array<DWORD, 2U> axes{dinput::objects()[1U].id | DIDFT_FFACTUATOR,
            dinput::objects()[0U].id | DIDFT_FFACTUATOR};
        std::array<LONG, 2U> directions{2, 3};
        DIEFFECT update{};
        update.dwSize = sizeof(update);
        update.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
        update.cAxes = counts.second;
        update.rgdwAxes = axes.data();
        update.rglDirection = directions.data();
        const DWORD selected = DIEP_AXES | DIEP_DIRECTION;
        if (no_restart) {
            REQUIRE(
                fixture.feedback->set(id, &update, selected | DIEP_NORESTART, nullptr, acquired) ==
                DIERR_EFFECTPLAYING);
            std::array<DWORD, 2U> original_axes{};
            DIEFFECT original{};
            original.dwSize = sizeof(original);
            original.dwFlags = DIEFF_OBJECTIDS;
            original.cAxes = 2U;
            original.rgdwAxes = original_axes.data();
            REQUIRE(fixture.feedback->get(id, &original, DIEP_AXES, nullptr) == DI_OK);
            CHECK(original.cAxes == counts.first);
            CHECK(original_axes[0U] == (dinput::objects()[0U].id | DIDFT_FFACTUATOR));
        }
        REQUIRE(fixture.feedback->set(id, &update, selected, nullptr, acquired) ==
                (playing ? DI_EFFECTRESTARTED : DI_OK));
        std::array<DWORD, 2U> returned_axes{};
        std::array<LONG, 2U> returned_directions{};
        DIEFFECT returned{};
        returned.dwSize = sizeof(returned);
        returned.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
        returned.cAxes = 2U;
        returned.rgdwAxes = returned_axes.data();
        returned.rglDirection = returned_directions.data();
        REQUIRE(fixture.feedback->get(id, &returned, selected, nullptr) == DI_OK);
        CHECK(returned.cAxes == counts.second);
        CHECK(returned_axes[0U] == axes[0U]);
        CHECK(returned_directions[0U] == directions[0U]);
        if (counts.second == 2U) {
            CHECK(returned_axes[1U] == axes[1U]);
            CHECK(returned_directions[1U] == directions[1U]);
        }
        fixture.feedback->release(id);
    }
}

TEST_CASE("dinput.feedback.norestart_rejects_static_updates_without_mutating_parameters",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwStartDelay = 100000U;
    CHECK(fixture.feedback->set(id, &update, DIEP_STARTDELAY | DIEP_NORESTART, nullptr, acquired) ==
          DIERR_EFFECTPLAYING);
    DIEFFECT result{};
    result.dwSize = sizeof(result);
    REQUIRE(fixture.feedback->get(id, &result, DIEP_STARTDELAY, nullptr) == DI_OK);
    CHECK(result.dwStartDelay == 0U);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    REQUIRE(fixture.feedback->set(id, &update, DIEP_STARTDELAY, nullptr, acquired) ==
            DI_EFFECTRESTARTED);
    REQUIRE(fixture.feedback->get(id, &result, DIEP_STARTDELAY, nullptr) == DI_OK);
    CHECK(result.dwStartDelay == update.dwStartDelay);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
}

TEST_CASE("dinput.feedback.norestart_preserves_elapsed_time_for_dynamic_parameters",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 3U}, acquired) == DI_OK);
    REQUIRE(fixture.wait_for_amplitude(0.8F));
    std::this_thread::sleep_for(10ms);
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwDuration = 1U;
    REQUIRE(fixture.feedback->set(id, &update, DIEP_DURATION | DIEP_NORESTART, nullptr, acquired) ==
            DI_OK);
    DWORD status{};
    REQUIRE(fixture.feedback->status(id, &status) == DI_OK);
    CHECK((status & DIEGES_PLAYING) == 0U);
    const auto current = fixture.trusted.public_authority().read_snapshot(fixture.target);
    REQUIRE(current.has_value());
    CHECK_FALSE(current->rumble.has_value());
}

TEST_CASE("dinput.feedback.staged_static_updates_require_restart_at_download",
    "[integration][dinput][force_feedback][parameters]") {
    feedback_fixture fixture;
    const auto id = fixture.create(&fixture);
    const auto acquired = fixture.acquisition();
    REQUIRE(fixture.feedback->start(id, {.iterations = 1U}, acquired) == DI_OK);
    DIEFFECT update{};
    update.dwSize = sizeof(update);
    update.dwStartDelay = 100000U;
    REQUIRE(
        fixture.feedback->set(id, &update, DIEP_STARTDELAY | DIEP_NODOWNLOAD, nullptr, acquired) ==
        DI_DOWNLOADSKIPPED);
    update.dwGain = 1000U;
    CHECK(fixture.feedback->set(id, &update, DIEP_GAIN | DIEP_NORESTART, nullptr, acquired) ==
          DIERR_EFFECTPLAYING);
    DIEFFECT result{};
    result.dwSize = sizeof(result);
    REQUIRE(fixture.feedback->get(id, &result, DIEP_GAIN, nullptr) == DI_OK);
    CHECK(result.dwGain == DI_FFNOMINALMAX);
    REQUIRE(fixture.feedback->download(id, acquired) == DI_OK);
    CHECK(fixture.feedback->download(id, acquired) == S_FALSE);
}

} // namespace
