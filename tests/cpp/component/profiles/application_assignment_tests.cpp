#include "application_test_support.hpp"

#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/xinput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

namespace {

using namespace inputnext;
using namespace inputnext::profiles;
using namespace inputnext::profiles_test_support;

TEST_CASE("profiles.application_uses_only_real_first_four_assignments",
    "[profiles][component][xinput][authority]") {
    application_fixture fixture;
    const auto desired = left_profile(0.45F);
    const auto prepared = fixture.app().replace_profile(desired);
    REQUIRE_FALSE(prepared.failure.has_value());
    CHECK(prepared.pending_contexts == 0U);

    std::array<test_connection, 5U> connections{};
    std::array<xinput::assignment, 4U> assignments{};
    for (auto& connection : connections) {
        connection = fixture.add_connection();
    }
    for (std::size_t index = 0U; index < assignments.size(); ++index) {
        assignments[index] = fixture.assign(connections[index]);
        CHECK(assignments[index].user_index == index);
    }

    const auto fifth = fixture.xinput().assign(connections[4U].connection.target,
        connections[4U].connection.reader);
    REQUIRE_FALSE(fifth.has_value());
    CHECK(fifth.error() == error::capacity_exceeded);
    const auto positions = fixture.xinput().positions();
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        REQUIRE(positions[index].has_value());
        CHECK(required_value(positions[index]) == assignments[index]);
    }

    const auto maintained = fixture.app().maintain();
    CHECK(maintained.pending_contexts == assignments.size());
    CHECK_FALSE(maintained.failure.has_value());
    for (std::size_t index = 0U; index < assignments.size(); ++index) {
        const auto observed = fixture.snapshot(connections[index].connection.target);
        REQUIRE(observed.left_trigger.has_value());
        const auto observed_left = required_value(observed.left_trigger);
        CHECK(observed_left.value == feedback_trigger(0.45F));
    }
    const auto unassigned_snapshot = fixture.snapshot(connections[4U].connection.target);
    CHECK(unassigned_snapshot.present_channels() == 0U);

    const auto closed = fixture.app().close();
    CHECK(closed.terminal);
    CHECK(closed.pending_contexts == 0U);
    CHECK_FALSE(closed.failure.has_value());
}

TEST_CASE("profiles.application_tracks_complete_assignment_and_exact_target_identity",
    "[profiles][component][xinput][authority]") {
    application_fixture fixture;
    auto old_connection = fixture.add_connection();
    const auto old_assignment = fixture.assign(old_connection);
    const auto installed = fixture.app().replace_profile(left_profile(0.35F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto old_snapshot = fixture.snapshot(old_connection.connection.target);
    REQUIRE(old_snapshot.left_trigger.has_value());
    const auto old_left = required_value(old_snapshot.left_trigger);
    const auto old_context = old_left.provenance.identity;

    auto replacement_connection = fixture.replace_connection(old_connection.connection.target);
    CHECK(
        replacement_connection.connection.target.device == old_connection.connection.target.device);
    CHECK(replacement_connection.connection.target.connection !=
          old_connection.connection.target.connection);
    REQUIRE(fixture.xinput().unassign(old_assignment).has_value());
    const auto replacement_assignment = fixture.assign(replacement_connection);
    CHECK(replacement_assignment.user_index == old_assignment.user_index);
    CHECK(replacement_assignment.id != old_assignment.id);
    CHECK(replacement_assignment.target == replacement_connection.connection.target);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto replacement_snapshot = fixture.snapshot(replacement_connection.connection.target);
    REQUIRE(replacement_snapshot.left_trigger.has_value());
    const auto replacement_left = required_value(replacement_snapshot.left_trigger);
    const auto replacement_context = replacement_left.provenance.identity;
    CHECK(replacement_context.target == replacement_connection.connection.target);
    CHECK(replacement_context != old_context);
    CHECK_FALSE(fixture.record(old_context).has_value());

    REQUIRE(fixture.xinput().unassign(replacement_assignment).has_value());
    const auto same_target_assignment = fixture.assign(replacement_connection);
    CHECK(same_target_assignment.user_index == replacement_assignment.user_index);
    CHECK(same_target_assignment.target == replacement_assignment.target);
    CHECK(same_target_assignment.id != replacement_assignment.id);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto same_target_snapshot = fixture.snapshot(replacement_connection.connection.target);
    REQUIRE(same_target_snapshot.left_trigger.has_value());
    const auto same_target_left = required_value(same_target_snapshot.left_trigger);
    CHECK(same_target_left.provenance.identity != replacement_context);
    CHECK_FALSE(fixture.record(replacement_context).has_value());

    const auto closed = fixture.app().close();
    CHECK(closed.pending_contexts == 0U);
    CHECK(closed.terminal);
}

TEST_CASE("profiles.application_waits_for_foreground_and_reopens_fresh_context",
    "[profiles][component][focus][authority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto installed = fixture.app().replace_profile(left_profile(0.8F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto initial = fixture.snapshot(connection.connection.target);
    REQUIRE(initial.left_trigger.has_value());
    const auto initial_left = required_value(initial.left_trigger);
    const auto initial_context = initial_left.provenance.identity;

    fixture.focus().set(focus::evidence::background);
    const auto background = fixture.app().maintain();
    CHECK(background.pending_contexts == 0U);
    const auto background_snapshot = fixture.snapshot(connection.connection.target);
    CHECK_FALSE(background_snapshot.left_trigger.has_value());

    fixture.focus().set(focus::evidence::unavailable);
    const auto unavailable = fixture.app().maintain();
    CHECK(unavailable.pending_contexts == 0U);
    const auto unavailable_snapshot = fixture.snapshot(connection.connection.target);
    CHECK_FALSE(unavailable_snapshot.left_trigger.has_value());

    fixture.focus().set(focus::evidence::foreground);
    const auto restored = fixture.app().maintain();
    CHECK(restored.pending_contexts == 1U);
    CHECK_FALSE(restored.failure.has_value());
    const auto restored_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(restored_snapshot.left_trigger.has_value());
    const auto restored_left = required_value(restored_snapshot.left_trigger);
    CHECK(restored_left.value == feedback_trigger(0.8F));
    CHECK(restored_left.provenance.identity != initial_context);

    const auto closed = fixture.app().close();
    CHECK(closed.pending_contexts == 0U);
    CHECK(closed.terminal);
}

TEST_CASE("profiles.application_drains_revocation_when_focus_changes_between_maintenance",
    "[profiles][component][focus][authority][lifecycle]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto installed = fixture.app().replace_profile(left_profile(0.8F));
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);

    const auto initially_applied = fixture.app().maintain();
    REQUIRE_FALSE(initially_applied.failure.has_value());
    CHECK(initially_applied.pending_contexts == 1U);
    const auto initial_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(initial_snapshot.left_trigger.has_value());
    const auto initial_left = required_value(initial_snapshot.left_trigger);
    auto previous_identity = initial_left.provenance.identity;

    for (std::size_t cycle = 0U; cycle < 3U; ++cycle) {
        fixture.focus().set(focus::evidence::background);
        // Let authority qualification see the new foreground epoch only on the next app pass.
        fixture.focus().set(focus::evidence::foreground);

        const auto restored = fixture.app().maintain();
        CHECK(restored.pending_contexts == 1U);
        CHECK_FALSE(restored.failure.has_value());

        const auto snapshot = fixture.snapshot(connection.connection.target);
        REQUIRE(snapshot.left_trigger.has_value());
        const auto left = required_value(snapshot.left_trigger);
        CHECK(left.value == feedback_trigger(0.8F));
        CHECK(left.provenance.identity != previous_identity);
        CHECK_FALSE(fixture.record(previous_identity).has_value());
        const auto current_record = fixture.record(left.provenance.identity);
        REQUIRE(current_record.has_value());
        CHECK(required_value(current_record).open);

        previous_identity = left.provenance.identity;
    }

    const auto closed = fixture.app().close();
    CHECK(closed.pending_contexts == 0U);
    CHECK(closed.terminal);
}

} // namespace
