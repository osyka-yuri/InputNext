#include "connection_test_support.hpp"

#include "rumble_evidence.hpp"
#include "rumble_sequence.hpp"
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace inputnext::gameinput_detail {
namespace {

using namespace test_support;

TEST_CASE("gameinput.output.rumble.probe_rejects_cached_neutral_then_accepts_low_completion",
    "[gameinput][output][integration][rumble][evidence]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false, true));
    const release_rumble_controls release{fixture.controls};
    fixture.controls->block_next_write();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    REQUIRE(fixture.controls->wait_for_write());

    std::optional<output::backend_progress> initial_in_flight;
    REQUIRE(wait_until([&fixture, &initial_in_flight] {
        initial_in_flight = fixture.connections->backend_progress(fixture.target);
        if (!initial_in_flight.has_value()) {
            return false;
        }
        const auto& progress = initial_in_flight.value();
        return progress.write_in_flight && progress.last_attempt.has_value();
    }));
    const auto initial_in_flight_snapshot = checked_value(initial_in_flight);
    const auto initial_attempt = checked_value(initial_in_flight_snapshot.last_attempt);

    fixture.controls->release_write();
    std::optional<output::backend_progress> initial_neutral;
    REQUIRE(wait_until([&fixture, &initial_neutral, initial_attempt] {
        initial_neutral = fixture.connections->backend_progress(fixture.target);
        if (!initial_neutral.has_value()) {
            return false;
        }
        const auto& progress = initial_neutral.value();
        if (progress.write_in_flight || !progress.last_completed_attempt.has_value()) {
            return false;
        }
        return progress.last_completed_attempt.value().id == initial_attempt;
    }));
    const auto initial_neutral_snapshot = checked_value(initial_neutral);
    const auto initial_neutral_attempt =
        checked_value(initial_neutral_snapshot.last_completed_attempt);
    const auto initial_baseline_attempt = checked_value(initial_neutral_snapshot.last_attempt);
    CHECK(initial_neutral_attempt.output.channels == output::rumble_mask);
    CHECK(initial_neutral_attempt.output.values.rumble == output::rumble_value{});
    CHECK(initial_baseline_attempt == initial_attempt);
    CHECK(initial_neutral_snapshot.observed_publication.valid());

    const auto assignment = checked_value(fixture.presentation->presentation().positions()[0U]);
    const auto before_status =
        fixture.presentation->presentation().get_rumble_output_status(assignment.user_index);
    REQUIRE(before_status.has_value());
    const auto low_motor_sequence =
        make_rumble_sequence(rumble_motor_selection{.low_motor = true, .high_motor = true});
    const auto* const low = low_motor_sequence.begin();
    fixture.controls->block_next_write();
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = low->left_motor,
        .wRightMotorSpeed = low->right_motor};
    const auto setter_result = fixture.presentation->presentation().set_state(xinput::flavor::v1_4,
        assignment.user_index,
        &vibration);
    REQUIRE(setter_result == ERROR_SUCCESS);
    fixture.output_executor->request_drive();
    REQUIRE(fixture.controls->wait_for_write());

    std::optional<output::backend_progress> skew_progress;
    output::logical_snapshot skew_logical{};
    REQUIRE(wait_until([&fixture, &skew_progress, &skew_logical, initial_attempt, low] {
        skew_progress = fixture.connections->backend_progress(fixture.target);
        auto logical = fixture.composition->public_authority().read_snapshot(fixture.target);
        if (!skew_progress.has_value() || !logical.has_value()) {
            return false;
        }
        skew_logical = logical.value();
        if (!skew_logical.rumble.has_value()) {
            return false;
        }
        const auto& progress = skew_progress.value();
        return progress.write_in_flight && progress.last_attempt.has_value() &&
               progress.last_attempt.value().value > initial_attempt.value &&
               progress.last_completed_attempt.has_value() &&
               progress.last_completed_attempt.value().id == initial_attempt &&
               skew_logical.rumble.value().value == normalized_value(*low);
    }));
    const auto skew_progress_snapshot = checked_value(skew_progress);
    const auto skew_attempt = checked_value(skew_progress_snapshot.last_attempt);
    const auto skew_completed_attempt =
        checked_value(skew_progress_snapshot.last_completed_attempt);
    const auto skew_logical_channel = checked_value(skew_logical.rumble);
    CHECK(skew_completed_attempt.output.channels == output::rumble_mask);
    CHECK(skew_completed_attempt.output.values.rumble == output::rumble_value{});
    CHECK(skew_attempt != initial_baseline_attempt);
    CHECK(skew_progress_snapshot.observed_publication !=
          initial_neutral_snapshot.observed_publication);

    const auto after_status =
        fixture.presentation->presentation().get_rumble_output_status(assignment.user_index);
    REQUIRE(after_status.has_value());
    CHECK(assess_logical_evidence(*low,
        before_status,
        after_status,
        setter_result,
        true,
        true,
        skew_logical_channel.value,
        skew_logical_channel.provenance.accepted_commit_order));

    rumble_native_match matched{};
    const auto skew_evidence = assess_native_evidence(*low,
        initial_neutral_snapshot.backend,
        initial_baseline_attempt,
        {.progress = skew_progress_snapshot,
            .logical_channel_present = true,
            .logical_value = skew_logical_channel.value},
        std::nullopt,
        matched);
    CHECK(skew_evidence == rumble_native_evidence::pending);
    CHECK(matched.attempt.value == 0U);

    fixture.controls->release_write();
    std::optional<output::backend_progress> accepted_low;
    REQUIRE(wait_until([&fixture, &accepted_low, initial_attempt, low] {
        accepted_low = fixture.connections->backend_progress(fixture.target);
        if (!accepted_low.has_value()) {
            return false;
        }
        const auto& progress = accepted_low.value();
        if (progress.write_in_flight || !progress.last_completed_attempt.has_value()) {
            return false;
        }
        const auto& attempt = progress.last_completed_attempt.value();
        return attempt.id.value > initial_attempt.value &&
               attempt.publication == progress.observed_publication &&
               attempt.output.channels == output::rumble_mask &&
               attempt.output.values.rumble == normalized_value(*low);
    }));
    const auto accepted_low_snapshot = checked_value(accepted_low);
    const auto accepted_low_attempt = checked_value(accepted_low_snapshot.last_completed_attempt);
    auto logical_low = fixture.composition->public_authority().read_snapshot(fixture.target);
    REQUIRE(logical_low.has_value());
    const auto logical_low_snapshot = logical_low.value();
    const auto logical_low_channel = checked_value(logical_low_snapshot.rumble);
    CHECK(assess_native_evidence(*low,
              initial_neutral_snapshot.backend,
              initial_baseline_attempt,
              {.progress = accepted_low_snapshot,
                  .logical_channel_present = true,
                  .logical_value = logical_low_channel.value},
              std::nullopt,
              matched) == rumble_native_evidence::accepted_newer_attempt);
    CHECK(matched.output == accepted_low_attempt.output);
    CHECK(matched.publication == accepted_low_attempt.publication);
    CHECK(matched.attempt == accepted_low_attempt.id);
    CHECK(matched.result == accepted_low_attempt.result);

    const auto native_value = fixture.controls->last_value();
    REQUIRE(native_value.has_value());
    CHECK(checked_value(native_value) == normalized_value(*low));

    fixture.presentation->close();
    fixture.connections->shutdown();
}

} // namespace
} // namespace inputnext::gameinput_detail
