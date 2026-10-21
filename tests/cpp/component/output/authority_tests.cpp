#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

TEST_CASE("output.authority_atomic_channels_and_owned_snapshot", "[component][output]") {
    authority_fixture fixture{};
    authority foreign{fixture.runtime, fixture.registry, fixture.focus_observer};
    const auto owner_empty_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    const auto foreign_empty_snapshot = foreign.read_snapshot(fixture.connection.target);
    REQUIRE(owner_empty_snapshot.has_value());
    REQUIRE(foreign_empty_snapshot.has_value());
    CHECK(owner_empty_snapshot->target == foreign_empty_snapshot->target);
    CHECK(owner_empty_snapshot->present_channels() == 0U);
    CHECK(foreign_empty_snapshot->present_channels() == 0U);
    CHECK(owner_empty_snapshot->logical_version == foreign_empty_snapshot->logical_version);
    CHECK(owner_empty_snapshot->incarnation != foreign_empty_snapshot->incarnation);
    const auto owner_empty_maintenance = fixture.owner.maintain(fixture.connection.target);
    const auto foreign_empty_maintenance = foreign.maintain(fixture.connection.target);
    REQUIRE(owner_empty_maintenance.has_value());
    REQUIRE(foreign_empty_maintenance.has_value());
    CHECK(owner_empty_maintenance->target == foreign_empty_maintenance->target);
    CHECK(owner_empty_maintenance->logical_version == foreign_empty_maintenance->logical_version);
    CHECK(owner_empty_maintenance->incarnation != foreign_empty_maintenance->incarnation);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    CHECK(opened->identity.incarnation.valid());
    const auto opening_info = fixture.owner.inspect(*opened);
    REQUIRE(opening_info.has_value());
    CHECK(opening_info->identity.incarnation == opened->identity.incarnation);
    REQUIRE(opening_info->foreground.has_value());

    auto first =
        inputnext::test::output::rumble_request({.channels = left_trigger_mask | rumble_mask,
            .sequence = 1U,
            .ttl = 5000ms,
            .low = 0.25F,
            .high = 0.75F});
    first.values.left_trigger.mode = trigger_mode::feedback;
    first.values.left_trigger.strength = 0.5F;
    first.values.left_trigger.frequency = 37.5F;
    const auto first_receipt = fixture.owner.submit(*opened, first);
    REQUIRE(first_receipt.has_value());
    CHECK(first_receipt->accepted_sequence == 1U);
    CHECK(first_receipt->resulting_revisions[0U] == 1U);
    CHECK(first_receipt->resulting_revisions[2U] == 1U);
    const auto duplicate_sequence = fixture.owner.submit(*opened, first);
    CHECK((!duplicate_sequence.has_value() &&
           duplicate_sequence.error() == output_error::sequence_rejected));

    const auto first_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(first_snapshot.has_value());
    CHECK(first_snapshot->incarnation == opened->identity.incarnation);
    CHECK(first_snapshot->present_channels() == (left_trigger_mask | rumble_mask));
    REQUIRE(first_snapshot->left_trigger.has_value());
    REQUIRE(first_snapshot->rumble.has_value());
    CHECK(
        first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{}).value.frequency ==
        37.5F);
    CHECK(first_snapshot->rumble.value_or(channel_snapshot<rumble_value>{}).value.low_frequency ==
          0.25F);
    CHECK(first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 1U);
    CHECK(first_snapshot->rumble.value_or(channel_snapshot<rumble_value>{})
              .provenance.accepted_sequence == 1U);
    const auto observations_before_refresh = fixture.focus_source().observations();
    REQUIRE(fixture.focus_source().wait_for_observations(observations_before_refresh + 1U));
    const auto refreshed_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(refreshed_snapshot.has_value());
    const auto refreshed_info = fixture.owner.inspect(*opened);
    REQUIRE(refreshed_info.has_value());
    CHECK(refreshed_info->foreground == opening_info->foreground);

    auto invalid_unselected = inputnext::test::output::rumble_request(
        {.channels = right_trigger_mask, .sequence = 2U, .ttl = 5s});
    invalid_unselected.values.rumble.high_frequency = std::numeric_limits<float>::quiet_NaN();
    const auto rejected = fixture.owner.submit(*opened, invalid_unselected);
    CHECK((!rejected.has_value() && rejected.error() == output_error::invalid_argument));
    const auto unchanged = fixture.owner.inspect(*opened);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->accepted_sequence == 1U);
    CHECK(unchanged->revisions == first_receipt->resulting_revisions);

    auto second = inputnext::test::output::rumble_request(
        {.channels = right_trigger_mask, .sequence = 2U, .ttl = 5s});
    second.values.right_trigger.mode = trigger_mode::weapon;
    second.values.right_trigger.strength = 0.8F;
    const auto second_receipt = fixture.owner.submit(*opened, second);
    REQUIRE(second_receipt.has_value());
    const auto second_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(second_snapshot.has_value());
    REQUIRE(second_snapshot->left_trigger.has_value());
    REQUIRE(second_snapshot->right_trigger.has_value());
    CHECK(second_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 1U);
    CHECK(second_snapshot->right_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 2U);

    const clear_request stale_clear{
        .selected_channels = left_trigger_mask | rumble_mask,
        .sequence = 3U,
        .expected_revisions = {1U, 0U, 0U, 0U},
    };
    const auto stale = fixture.owner.clear(*opened, stale_clear);
    CHECK((!stale.has_value() && stale.error() == output_error::stale_revision));
    const auto after_stale = fixture.owner.inspect(*opened);
    REQUIRE(after_stale.has_value());
    CHECK(after_stale->accepted_sequence == 2U);
    CHECK(after_stale->revisions == second_receipt->resulting_revisions);

    const clear_request clear{
        .selected_channels = left_trigger_mask | rumble_mask,
        .sequence = 3U,
        .expected_revisions = {1U, 0U, 1U, 0U},
    };
    const auto cleared = fixture.owner.clear(*opened, clear);
    REQUIRE(cleared.has_value());
    const auto after_clear = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_clear.has_value());
    CHECK(after_clear->present_channels() == right_trigger_mask);
    CHECK(after_clear->left_trigger == std::nullopt);
    CHECK(after_clear->right_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 2U);

    CHECK(first_snapshot->present_channels() == (left_trigger_mask | rumble_mask));
    CHECK(
        first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{}).value.frequency ==
        37.5F);
    CHECK(first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 1U);

    const auto closed = fixture.owner.close(*opened, opened->identity.id.value);
    REQUIRE(closed.has_value());
    REQUIRE(fixture.owner.acknowledge(*closed).has_value());
    const auto empty_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(empty_snapshot.has_value());
    CHECK(empty_snapshot->present_channels() == 0U);
    CHECK(empty_snapshot->incarnation == opened->identity.incarnation);
    const auto empty_maintenance = fixture.owner.maintain(fixture.connection.target);
    REQUIRE(empty_maintenance.has_value());
    CHECK(empty_maintenance->incarnation == opened->identity.incarnation);
    const auto reused = fixture.owner.open(fixture.connection.target);
    REQUIRE(reused.has_value());
    CHECK(reused->identity.id.value > opened->identity.id.value);
    CHECK(first_snapshot->present_channels() == (left_trigger_mask | rumble_mask));
    CHECK(
        first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{}).value.frequency ==
        37.5F);
    CHECK(first_snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.identity == opened->identity);
}

TEST_CASE("output.authority_cleanup_remains_available_at_logical_headroom_limit",
    "[component][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto accepted = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.2F, .high = 0.3F}));
    REQUIRE(accepted.has_value());
    inputnext::output::detail::authority_test_access::set_logical_version(fixture.owner,
        std::numeric_limits<std::uint64_t>::max() - 2U);

    const auto blocked = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.8F, .high = 0.9F}));
    CHECK((!blocked.has_value() && blocked.error() == output_error::logical_version_exhausted));
    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 1U);
    const auto closed = fixture.owner.close(*opened, opened->identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(closed->logical_revocation_version == std::numeric_limits<std::uint64_t>::max() - 1U);
    REQUIRE(fixture.owner.acknowledge(*closed).has_value());
}

TEST_CASE("output.identity_revision_and_deadline_exhaustion_do_not_wrap_or_partially_commit",
    "[component][output]") {
    authority_fixture fixture{};
    authority final_id_authority{fixture.runtime,
        fixture.registry,
        fixture.focus_observer,
        {.next_context_id = std::numeric_limits<std::uint64_t>::max(), .logical_version = 0U}};
    const auto final_id = final_id_authority.open(fixture.connection.target);
    REQUIRE(final_id.has_value());
    CHECK(final_id->identity.id.value == std::numeric_limits<std::uint64_t>::max());
    const auto id_exhausted = final_id_authority.open(fixture.connection.target);
    CHECK((!id_exhausted.has_value() && id_exhausted.error() == output_error::identity_exhausted));
    const auto final_receipt = final_id_authority.close(*final_id, final_id->identity.id.value);
    REQUIRE(final_receipt.has_value());
    REQUIRE(final_id_authority.acknowledge(*final_receipt).has_value());
    const auto remains_exhausted = final_id_authority.open(fixture.connection.target);
    CHECK((!remains_exhausted.has_value() &&
           remains_exhausted.error() == output_error::identity_exhausted));

    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    REQUIRE(inputnext::output::detail::authority_test_access::set_channel_revision(fixture.owner,
        opened->identity,
        channel::left_trigger,
        std::numeric_limits<std::uint64_t>::max()));
    const auto request = inputnext::test::output::rumble_request(
        {.channels = left_trigger_mask, .sequence = 1U, .ttl = 1ms});
    const auto revision_exhausted = fixture.owner.submit(*opened, request);
    CHECK((!revision_exhausted.has_value() &&
           revision_exhausted.error() == output_error::revision_exhausted));
    const auto unchanged = fixture.owner.inspect(*opened);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->accepted_sequence == 0U);
    CHECK(unchanged->revisions[0U] == std::numeric_limits<std::uint64_t>::max());

    std::atomic<std::int64_t> const ticks{std::numeric_limits<std::int64_t>::max() - 10};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto overflow = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms}));
    CHECK((!overflow.has_value() && overflow.error() == output_error::deadline_overflow));
    const auto still_unchanged = fixture.owner.inspect(*opened);
    REQUIRE(still_unchanged.has_value());
    CHECK(still_unchanged->accepted_sequence == 0U);
}

} // namespace
