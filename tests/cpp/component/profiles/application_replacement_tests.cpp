#include "application_test_support.hpp"

#include "output/authority_test_access.hpp"
#include "profiles/application.hpp"
#include "profiles/model.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

namespace {

using namespace inputnext;
using namespace inputnext::profiles;
using namespace inputnext::profiles_test_support;

TEST_CASE("profiles.application_empty_default_and_replacement_allocate_no_context",
    "[profiles][component][authority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto before = output::detail::authority_test_access::logical_version(fixture.authority());

    const auto default_state = fixture.app().maintain();
    CHECK(default_state.pending_contexts == 0U);
    CHECK_FALSE(default_state.failure.has_value());
    CHECK(output::detail::authority_test_access::logical_version(fixture.authority()) == before);

    const auto empty_replacement = fixture.app().replace_profile(profile{});
    CHECK(empty_replacement.pending_contexts == 0U);
    CHECK_FALSE(empty_replacement.failure.has_value());
    CHECK_FALSE(empty_replacement.terminal);
    CHECK(output::detail::authority_test_access::logical_version(fixture.authority()) == before);
    CHECK(fixture.snapshot(connection.connection.target).present_channels() == 0U);

    const auto closed = fixture.app().close();
    CHECK(closed.terminal);
    CHECK(closed.pending_contexts == 0U);
}

TEST_CASE("profiles.application_reconciles_preassignment_settings_without_republishing",
    "[profiles][component][xinput][authority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    const auto target = connection.connection.target;
    const auto desired =
        profile{.left_trigger = feedback_trigger(0.4F), .right_trigger = feedback_trigger(0.7F)};

    const auto initial_version =
        output::detail::authority_test_access::logical_version(fixture.authority());
    const auto prepared = fixture.app().replace_profile(desired);
    CHECK(prepared.pending_contexts == 0U);
    CHECK_FALSE(prepared.failure.has_value());
    CHECK_FALSE(prepared.terminal);
    CHECK(output::detail::authority_test_access::logical_version(fixture.authority()) ==
          initial_version);

    const auto assigned = fixture.assign(connection);
    const auto applied = fixture.app().maintain();
    CHECK(applied.pending_contexts == 1U);
    CHECK_FALSE(applied.failure.has_value());
    const auto first_snapshot = fixture.snapshot(target);
    REQUIRE(first_snapshot.left_trigger.has_value());
    REQUIRE(first_snapshot.right_trigger.has_value());
    const auto first_left = required_value(first_snapshot.left_trigger);
    const auto first_right = required_value(first_snapshot.right_trigger);
    CHECK(first_left.value == feedback_trigger(0.4F));
    CHECK(first_right.value == feedback_trigger(0.7F));
    CHECK(first_left.provenance.identity.target == target);
    CHECK(first_left.provenance.identity.internal_owner == application_options.owner);

    const auto opened_record = fixture.record(first_left.provenance.identity);
    REQUIRE(opened_record.has_value());
    const auto before_repeat = required_value(opened_record);
    const auto repeated = fixture.app().maintain();
    CHECK(repeated.pending_contexts == 1U);
    CHECK_FALSE(repeated.failure.has_value());
    const auto repeated_snapshot = fixture.snapshot(target);
    REQUIRE(repeated_snapshot.left_trigger.has_value());
    REQUIRE(repeated_snapshot.right_trigger.has_value());
    const auto repeated_left = required_value(repeated_snapshot.left_trigger);
    const auto repeated_right = required_value(repeated_snapshot.right_trigger);
    CHECK(repeated_left.provenance.identity == first_left.provenance.identity);
    CHECK(repeated_right.provenance.identity == first_right.provenance.identity);
    const auto after_repeat = fixture.record(first_left.provenance.identity);
    REQUIRE(after_repeat.has_value());
    const auto repeated_record = required_value(after_repeat);
    CHECK(repeated_record.accepted_sequence == before_repeat.accepted_sequence);
    CHECK(repeated_record.revisions == before_repeat.revisions);
    CHECK(repeated_record.logical_version == before_repeat.logical_version);
    CHECK(repeated_record.accepted_sequence == 1U);
    CHECK(repeated_record.revisions[0U] == 1U);
    CHECK(repeated_record.revisions[1U] == 1U);
    CHECK(assigned.user_index == 0U);
}

TEST_CASE("profiles.application_replacement_removes_channels_and_preserves_explicit_off",
    "[profiles][component][authority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));

    const auto both =
        profile{.left_trigger = feedback_trigger(0.25F), .right_trigger = feedback_trigger(0.75F)};
    const auto installed = fixture.app().replace_profile(both);
    REQUIRE_FALSE(installed.failure.has_value());
    CHECK(installed.pending_contexts == 1U);
    REQUIRE(fixture.app().maintain().pending_contexts == 1U);
    const auto initial = fixture.snapshot(connection.connection.target);
    REQUIRE(initial.left_trigger.has_value());
    REQUIRE(initial.right_trigger.has_value());
    const auto initial_left = required_value(initial.left_trigger);
    const auto initial_right = required_value(initial.right_trigger);
    const auto initial_identity = initial_left.provenance.identity;
    CHECK(initial_right.provenance.identity == initial_identity);

    const auto equal = fixture.app().replace_profile(both);
    CHECK(equal.pending_contexts == 1U);
    CHECK_FALSE(equal.failure.has_value());
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto equal_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(equal_snapshot.left_trigger.has_value());
    const auto equal_left = required_value(equal_snapshot.left_trigger);
    CHECK(equal_left.provenance.identity == initial_identity);
    const auto equal_record = fixture.record(initial_identity);
    REQUIRE(equal_record.has_value());
    const auto equal_record_value = required_value(equal_record);
    CHECK(equal_record_value.accepted_sequence == 1U);
    CHECK(equal_record_value.revisions[0U] == 1U);
    CHECK(equal_record_value.revisions[1U] == 1U);

    const auto left_only =
        profile{.left_trigger = feedback_trigger(0.5F), .right_trigger = std::nullopt};
    const auto removed = fixture.app().replace_profile(left_only);
    CHECK(removed.pending_contexts == 1U);
    CHECK_FALSE(removed.failure.has_value());
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto left_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(left_snapshot.left_trigger.has_value());
    const auto replacement_left = required_value(left_snapshot.left_trigger);
    CHECK_FALSE(left_snapshot.right_trigger.has_value());
    CHECK(replacement_left.value == feedback_trigger(0.5F));
    CHECK(replacement_left.provenance.identity != initial_identity);

    const auto explicit_off =
        profile{.left_trigger = output::trigger_value{}, .right_trigger = std::nullopt};
    const auto off_replacement = fixture.app().replace_profile(explicit_off);
    REQUIRE_FALSE(off_replacement.failure.has_value());
    CHECK(off_replacement.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto off_snapshot = fixture.snapshot(connection.connection.target);
    REQUIRE(off_snapshot.left_trigger.has_value());
    const auto off_left = required_value(off_snapshot.left_trigger);
    CHECK(off_left.value.mode == output::trigger_mode::off);
    CHECK(off_left.provenance.identity != replacement_left.provenance.identity);

    const auto emptied = fixture.app().replace_profile(profile{});
    CHECK(emptied.pending_contexts == 0U);
    CHECK_FALSE(emptied.failure.has_value());
    CHECK_FALSE(emptied.terminal);
    const auto empty_snapshot = fixture.snapshot(connection.connection.target);
    CHECK(empty_snapshot.present_channels() == 0U);
}

TEST_CASE("profiles.application_invalid_programmatic_profile_keeps_previous_owner",
    "[profiles][component][validation][authority]") {
    application_fixture fixture;
    const auto connection = fixture.add_connection();
    static_cast<void>(fixture.assign(connection));
    const auto accepted = fixture.app().replace_profile(left_profile(0.6F));
    REQUIRE_FALSE(accepted.failure.has_value());
    CHECK(accepted.pending_contexts == 1U);
    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());

    const auto before = fixture.snapshot(connection.connection.target);
    REQUIRE(before.left_trigger.has_value());
    const auto before_left = required_value(before.left_trigger);
    const auto old_identity = before_left.provenance.identity;
    const auto before_record = fixture.record(old_identity);
    REQUIRE(before_record.has_value());
    const auto before_record_value = required_value(before_record);

    auto invalid = profile{.left_trigger = feedback_trigger(0.1F), .right_trigger = std::nullopt};
    invalid.right_trigger = feedback_trigger(std::nanf(""));
    const auto rejected = fixture.app().replace_profile(invalid);
    REQUIRE(rejected.failure.has_value());
    const auto validation_failure = required_value(rejected.failure);
    CHECK(validation_failure.operation == application_operation::validate);
    CHECK(validation_failure.code == output::error::invalid_argument);
    CHECK_FALSE(rejected.terminal);
    CHECK(rejected.pending_contexts == 1U);

    REQUIRE_FALSE(fixture.app().maintain().failure.has_value());
    const auto after = fixture.snapshot(connection.connection.target);
    REQUIRE(after.left_trigger.has_value());
    const auto after_left = required_value(after.left_trigger);
    CHECK(after_left.provenance.identity == old_identity);
    CHECK(after_left.value == feedback_trigger(0.6F));
    const auto after_record = fixture.record(old_identity);
    REQUIRE(after_record.has_value());
    const auto after_record_value = required_value(after_record);
    CHECK(after_record_value.accepted_sequence == before_record_value.accepted_sequence);
    CHECK(after_record_value.revisions == before_record_value.revisions);
}

} // namespace
