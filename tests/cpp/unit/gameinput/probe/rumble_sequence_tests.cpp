#include "gameinput_probe/output/rumble/sequence.hpp"

#include "gameinput_probe/output/rumble/evidence.hpp"
#include "gameinput_probe/output/rumble/report.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

[[nodiscard]] output::backend_handle backend_for(connection_target target) {
    output::authority_incarnation authority{};
    authority.bytes[0] = std::byte{1U};
    return {.target = target,
        .incarnation = {.owner = {.authority = authority, .id = output::execution_id{2U}},
            .id = output::backend_id{3U}}};
}

[[nodiscard]] connection_target target_for_test() {
    runtime_id runtime{};
    runtime.bytes[0] = std::byte{1U};
    return {.runtime = runtime, .device = device_key{4U}, .connection = connection_id{5U}};
}

[[nodiscard]] rumble_sequence both_motor_sequence() {
    return make_rumble_sequence({.low_motor = true, .high_motor = true});
}

[[nodiscard]] output::completed_attempt checked_completed_attempt(
    const rumble_native_observation& observation) {
    const auto attempt = observation.progress.last_completed_attempt;
    if (!attempt.has_value()) {
        throw std::logic_error("expected a completed native attempt in the test observation");
    }
    return *attempt;
}

void set_completed_output(rumble_native_observation& observation,
    const output::native_output native_output) {
    auto completed = checked_completed_attempt(observation);
    completed.output = native_output;
    observation.progress.last_completed_attempt = completed;
}

[[nodiscard]] rumble_native_observation accepted(output::backend_handle backend,
    const rumble_step& step,
    const std::uint64_t publication,
    const std::uint64_t attempt,
    const bool channel_present = true) {
    const auto value = channel_present ? normalized_value(step) : output::rumble_value{};
    const output::backend_progress progress{.backend = backend,
        .phase = output::backend_phase::current,
        .represented_channels = output::rumble_mask,
        .observed_publication = output::publication_id{publication},
        .delivery = output::delivery_state::setter_accepted,
        .last_attempt = output::attempt_id{attempt},
        .last_completed_attempt = output::completed_attempt{.id = output::attempt_id{attempt},
            .publication = output::publication_id{publication},
            .result = {.status = output::native_status::accepted},
            .output = {.channels = output::rumble_mask, .values = {.rumble = value}}}};
    return {.progress = progress,
        .logical_channel_present = channel_present,
        .logical_value = value};
}

TEST_CASE("gameinput.probe.rumble.sequence_is_bounded_and_modest",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto observed = sequence.steps();

    REQUIRE(observed.size() == 7U);
    for (const auto& step : observed) {
        CHECK((!step.instruction.empty()));
    }
    CHECK((observed[0U].name == "low_motor"));
    CHECK((observed[1U].name == "high_motor"));
    CHECK((observed[2U].name == "zero"));
    CHECK((observed[3U].name == "disable"));
    CHECK((observed[4U].name == "disabled_cached_request"));
    CHECK((observed[5U].name == "reenable_restore"));
    CHECK((observed[6U].name == "final_zero"));
    CHECK(observed[0U].instruction == "Feel the low-frequency vibration.");
    CHECK(observed[1U].instruction == "Feel the high-frequency vibration.");
    CHECK(observed[4U].instruction == "Check that no rumble starts while output is disabled.");
    CHECK(normalized_value(observed[0U]).low_frequency < 0.35F);
    CHECK(normalized_value(observed[1U]).high_frequency < 0.35F);
    CHECK(normalized_value(observed[2U]) == output::rumble_value{});
    CHECK(observed[4U].expects_native_output == false);
}

TEST_CASE("gameinput.probe.rumble.sequence_low_only_omits_high_motor",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = make_rumble_sequence({.low_motor = true, .high_motor = false});
    const auto steps = sequence.steps();

    REQUIRE(steps.size() == 6U);
    CHECK((steps[0U].name == "low_motor"));
    CHECK((steps[1U].name == "zero"));
    CHECK((steps[3U].name == "disabled_cached_request"));
    CHECK(steps[3U].left_motor != 0U);
    CHECK(steps[3U].right_motor == 0U);
    CHECK((steps[4U].name == "reenable_restore"));
    CHECK(steps[4U].left_motor == steps[3U].left_motor);
    CHECK(steps[4U].right_motor == 0U);
    CHECK((steps[5U].name == "final_zero"));
}

TEST_CASE("gameinput.probe.rumble.sequence_both_motors_preserve_combined_restore",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto steps = sequence.steps();

    REQUIRE(steps.size() == 7U);
    CHECK((steps[0U].name == "low_motor"));
    CHECK((steps[1U].name == "high_motor"));
    CHECK(steps[4U].left_motor != 0U);
    CHECK(steps[4U].right_motor != 0U);
    CHECK(steps[5U].left_motor == steps[4U].left_motor);
    CHECK(steps[5U].right_motor == steps[4U].right_motor);
}

TEST_CASE("gameinput.probe.rumble.sequence_high_only_omits_low_motor",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = make_rumble_sequence({.low_motor = false, .high_motor = true});
    const auto steps = sequence.steps();

    REQUIRE(steps.size() == 6U);
    CHECK((steps[0U].name == "high_motor"));
    CHECK((steps[1U].name == "zero"));
    CHECK((steps[3U].name == "disabled_cached_request"));
    CHECK(steps[3U].left_motor == 0U);
    CHECK(steps[3U].right_motor != 0U);
    CHECK((steps[4U].name == "reenable_restore"));
    CHECK(steps[4U].left_motor == 0U);
    CHECK(steps[4U].right_motor == steps[3U].right_motor);
    CHECK((steps[5U].name == "final_zero"));
}

TEST_CASE("gameinput.probe.rumble.sequence_no_motors_is_empty",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = make_rumble_sequence({.low_motor = false, .high_motor = false});
    CHECK(sequence.steps().empty());
}

TEST_CASE("gameinput.probe.rumble.readiness_accepts_either_reported_motor",
    "[unit][gameinput][probe][rumble]") {
    rumble_readiness readiness{.assignment = xinput::assignment{},
        .configured = true,
        .low_motor = true,
        .high_motor = false};
    CHECK(readiness.supported());

    readiness.low_motor = false;
    readiness.high_motor = true;
    CHECK(readiness.supported());

    readiness.high_motor = false;
    CHECK(!readiness.supported());
}

TEST_CASE("gameinput.probe.rumble.stale_neutral_attempt_does_not_confirm_nonzero",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const auto backend = backend_for(target_for_test());
    auto observation = accepted(backend, *low, 10U, 14U);
    set_completed_output(observation, {.channels = output::rumble_mask, .values = {.rumble = {}}});
    const auto prior = rumble_native_match{.backend = backend,
        .output = {.channels = output::rumble_mask,
            .values = {.rumble = normalized_value(*(low + 2))}},
        .publication = output::publication_id{8U},
        .attempt = output::attempt_id{11U},
        .result = {.status = output::native_status::accepted}};
    auto matched = prior;

    set_completed_output(observation,
        {.channels = output::rumble_mask | output::leds_mask, .values = {.rumble = {}}});
    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{13U},
              observation,
              prior,
              matched) == rumble_native_evidence::pending);
    CHECK(matched == prior);

    set_completed_output(observation, {.channels = output::rumble_mask, .values = {.rumble = {}}});
    const auto evidence =
        assess_native_evidence(*low, backend, output::attempt_id{13U}, observation, prior, matched);

    CHECK(evidence == rumble_native_evidence::pending);
    CHECK(matched == prior);
}

TEST_CASE("gameinput.probe.rumble.accepts_newer_matching_attempt",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const auto backend = backend_for(target_for_test());
    const auto observation = accepted(backend, *low, 10U, 14U);
    rumble_native_match matched{};

    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{13U},
              observation,
              std::nullopt,
              matched) == rumble_native_evidence::accepted_newer_attempt);
    CHECK(matched.backend == backend);
    CHECK(matched.output.channels == output::rumble_mask);
    CHECK(matched.output.values.rumble == normalized_value(*low));
    CHECK(matched.publication == output::publication_id{10U});
    CHECK(matched.attempt == output::attempt_id{14U});

    const auto accepted_match = matched;
    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{14U},
              observation,
              std::nullopt,
              matched) == rumble_native_evidence::pending);
    CHECK(matched == accepted_match);
}

TEST_CASE("gameinput.probe.rumble.pre_step_attempt_is_not_new_after_publication_change",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const auto backend = backend_for(target_for_test());
    const auto observation = accepted(backend, *low, 11U, 14U);
    rumble_native_match matched{};

    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{14U},
              observation,
              std::nullopt,
              matched) == rumble_native_evidence::pending);
    CHECK(!matched.publication.valid());
}

TEST_CASE("gameinput.probe.rumble.rejected_evidence_preserves_prior_match",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const auto backend = backend_for(target_for_test());
    const auto observation = accepted(backend, *low, 10U, 14U);
    auto wrong_backend = backend;
    wrong_backend.incarnation.id = output::backend_id{99U};
    const auto prior = rumble_native_match{.backend = backend,
        .output = checked_completed_attempt(observation).output,
        .publication = output::publication_id{8U},
        .attempt = output::attempt_id{11U},
        .result = {.status = output::native_status::accepted}};
    auto matched = prior;

    CHECK(assess_native_evidence(*low,
              wrong_backend,
              output::attempt_id{13U},
              observation,
              prior,
              matched) == rumble_native_evidence::rejected);
    CHECK(matched == prior);
}

TEST_CASE("gameinput.probe.rumble.repeated_value_requires_existing_match",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const auto backend = backend_for(target_for_test());
    const auto observation = accepted(backend, *low, 10U, 14U);
    const auto prior = rumble_native_match{.backend = backend,
        .output = {.channels = output::rumble_mask, .values = {.rumble = normalized_value(*low)}},
        .publication = output::publication_id{10U},
        .attempt = output::attempt_id{14U},
        .result = {.status = output::native_status::accepted}};
    rumble_native_match matched{};

    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{14U},
              observation,
              prior,
              matched) == rumble_native_evidence::historical_reuse);
    CHECK(matched.publication == prior.publication);
    CHECK(matched.attempt == prior.attempt);
    CHECK(matched.output == prior.output);

    auto false_result = prior;
    false_result.result = {.status = output::native_status::retryable_failure};
    CHECK(assess_native_evidence(*low,
              backend,
              output::attempt_id{14U},
              observation,
              false_result,
              matched) == rumble_native_evidence::pending);
    CHECK(matched == prior);
}

TEST_CASE("gameinput.probe.rumble.logical_evidence_matches_commit_and_request",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const low = sequence.begin();
    const xinput::rumble_output_status before{.configured = true,
        .desired_state_cached = true,
        .request_revision = 20U,
        .logical_version = 8U};
    const xinput::rumble_output_status after{.configured = true,
        .desired_state_cached = true,
        .request_revision = 21U,
        .logical_version = 9U};

    CHECK(assess_logical_evidence(*low,
        before,
        after,
        ERROR_SUCCESS,
        true,
        true,
        normalized_value(*low),
        9U));
    CHECK(!assess_logical_evidence(*low,
        before,
        after,
        ERROR_SUCCESS,
        true,
        true,
        normalized_value(*low),
        8U));
    CHECK(!assess_logical_evidence(*low,
        before,
        xinput::rumble_output_status{.configured = true,
            .desired_state_cached = true,
            .request_revision = 20U,
            .logical_version = 9U},
        ERROR_SUCCESS,
        true,
        true,
        normalized_value(*low),
        9U));
}

TEST_CASE("gameinput.probe.rumble.disabled_cache_has_no_logical_channel",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const disable = sequence.begin() + 3;
    const xinput::rumble_output_status before{.configured = true,
        .desired_state_cached = true,
        .request_revision = 30U,
        .logical_version = 9U};
    const xinput::rumble_output_status disabled{.configured = true,
        .desired_state_cached = true,
        .request_revision = 30U};
    CHECK(assess_logical_evidence(*disable,
        before,
        disabled,
        std::nullopt,
        true,
        false,
        {},
        std::nullopt));

    const auto* const cache_request = disable + 1;
    const xinput::rumble_output_status cached{.configured = true,
        .desired_state_cached = true,
        .request_revision = 31U};
    CHECK(assess_logical_evidence(*cache_request,
        disabled,
        cached,
        ERROR_SUCCESS,
        true,
        false,
        {},
        std::nullopt));
    CHECK(!assess_logical_evidence(*cache_request,
        disabled,
        disabled,
        ERROR_SUCCESS,
        true,
        false,
        {},
        std::nullopt));
}

TEST_CASE("gameinput.probe.rumble.reenable_requires_current_restore_projection",
    "[unit][gameinput][probe][rumble]") {
    const auto sequence = both_motor_sequence();
    const auto* const restore = sequence.begin() + 5;
    const xinput::rumble_output_status restored{.configured = true,
        .desired_state_cached = true,
        .restore_pending = false,
        .last_restore_error = ERROR_SUCCESS,
        .request_revision = 45U,
        .logical_version = 13U};

    CHECK(assess_logical_evidence(*restore,
        std::nullopt,
        restored,
        std::nullopt,
        true,
        true,
        normalized_value(*restore),
        13U));
    CHECK(!assess_logical_evidence(*restore,
        std::nullopt,
        xinput::rumble_output_status{.configured = true,
            .desired_state_cached = true,
            .restore_pending = true,
            .logical_version = 13U},
        std::nullopt,
        true,
        true,
        normalized_value(*restore),
        13U));
}

TEST_CASE("gameinput.probe.rumble.report_separates_assignment_and_native_evidence",
    "[unit][gameinput][probe][rumble]") {
    const auto target = target_for_test();
    const auto report = format_rumble_step({.step = "final_zero",
        .target = target,
        .assignment_id = 17U,
        .request_revision = 21U,
        .logical_channel_version = std::nullopt,
        .logical_result = 0U,
        .logical_call_made = true,
        .logical_state_confirmed = true,
        .assignment_live = true,
        .foreground = true,
        .low_motor_available = true,
        .high_motor_available = false,
        .capabilities_confirmed = true,
        .native_evidence = rumble_native_evidence::accepted_newer_attempt,
        .native_match = rumble_native_match{.backend = backend_for(target),
            .output = {.channels = output::rumble_mask,
                .values = {.rumble = {.low_frequency = 0.25F, .high_frequency = 0.0F}}},
            .publication = output::publication_id{23U},
            .attempt = output::attempt_id{25U},
            .result = {.status = output::native_status::accepted}}});

    CHECK(report.contains("key=4 connection=5 assignment=17 request_revision=21"));
    CHECK(report.contains("logical_state=true assignment_live=true foreground=true"));
    CHECK(report.contains("low_cap=true high_cap=false capabilities=true"));
    CHECK(report.contains("native_evidence=setter_accepted_newer_matching_attempt"));
    CHECK(report.contains("publication=23 attempt=25"));
    CHECK(report.contains("native_channels=4 native_rumble=0.250000,0.000000"));
    CHECK(!report.contains("physical="));
}

} // namespace
