#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

template <typename Value>
concept has_internal_issuer =
    requires(Value& value, internal_context_options options) { value.make_producer(options); };

template <typename Value>
concept has_rvalue_public_authority =
    requires(Value&& value) { std::move(value).public_authority(); };

static_assert(!std::is_constructible_v<inputnext::output::detail::composition, authority&>);
static_assert(!std::is_constructible_v<inputnext::output::detail::internal_producer,
    authority&,
    const internal_context_options&>);
static_assert(!std::is_move_constructible_v<inputnext::output::detail::composition>);
static_assert(!has_internal_issuer<authority>);
static_assert(!has_rvalue_public_authority<inputnext::output::detail::composition>);

TEST_CASE("output.internal_producer_checks_complete_grant_before_maintenance_and_ack",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const internal_context_options old_lower_options{
        .owner = internal_owner_key{991U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto old_lower =
        inputnext::test::output::make_internal_producer(fixture.trusted, old_lower_options);
    const auto old_lower_context = old_lower.open(fixture.connection.target);
    REQUIRE(old_lower_context.has_value());
    const auto old_lower_close =
        old_lower.close(*old_lower_context, old_lower_context->identity.id.value);
    REQUIRE(old_lower_close.has_value());
    REQUIRE(old_lower.acknowledge(*old_lower_close).has_value());

    const internal_context_options current_options{
        .owner = old_lower_options.owner,
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto current =
        inputnext::test::output::make_internal_producer(fixture.trusted, current_options);
    const auto current_context = current.open(fixture.connection.target);
    REQUIRE(current_context.has_value());
    const auto current_submit = current.submit(*current_context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.3F, .high = 0.5F}));
    REQUIRE(current_submit.has_value());

    auto wrong_ttl_options = current_options;
    wrong_ttl_options.ttl = ttl_permission::finite_or_indefinite;
    auto wrong_ttl =
        inputnext::test::output::make_internal_producer(fixture.trusted, wrong_ttl_options);
    auto wrong_foreground_options = current_options;
    wrong_foreground_options.foreground = foreground_policy::foreground_bound;
    auto wrong_foreground =
        inputnext::test::output::make_internal_producer(fixture.trusted, wrong_foreground_options);

    ticks.store(std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms).count(),
        std::memory_order_release);
    const auto wrong_role_open = old_lower.open(fixture.connection.target);
    CHECK((
        !wrong_role_open.has_value() && wrong_role_open.error() == output_error::invalid_argument));
    const auto wrong_priority_submit = old_lower.submit(*current_context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 1ms, .low = 0.7F, .high = 0.9F}));
    const clear_request wrong_ttl_clear{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = current_submit->resulting_revisions,
    };
    const auto wrong_ttl_clear_result = wrong_ttl.clear(*current_context, wrong_ttl_clear);
    const auto wrong_foreground_close =
        wrong_foreground.close(*current_context, current_context->identity.id.value);
    CHECK((!wrong_priority_submit.has_value() &&
           wrong_priority_submit.error() == output_error::stale_context));
    CHECK((!wrong_ttl_clear_result.has_value() &&
           wrong_ttl_clear_result.error() == output_error::stale_context));
    CHECK((!wrong_foreground_close.has_value() &&
           wrong_foreground_close.error() == output_error::stale_context));
    const auto unchanged =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            current_context->identity);
    REQUIRE(unchanged.has_value());
    const auto unchanged_state =
        unchanged.value_or(inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(unchanged_state.open);
    CHECK(unchanged_state.accepted_sequence == 1U);
    CHECK(unchanged_state.revisions == current_submit->resulting_revisions);
    CHECK(unchanged_state.active[2U]);

    const auto retained = current.close(*current_context, current_context->identity.id.value);
    REQUIRE(retained.has_value());
    const auto public_ack = fixture.owner.acknowledge(*retained);
    const auto wrong_priority_ack = old_lower.acknowledge(*retained);
    const auto wrong_ttl_ack = wrong_ttl.acknowledge(*retained);
    const auto wrong_foreground_ack = wrong_foreground.acknowledge(*retained);
    CHECK((!public_ack.has_value() && public_ack.error() == output_error::stale_context));
    CHECK((!wrong_priority_ack.has_value() &&
           wrong_priority_ack.error() == output_error::stale_context));
    CHECK((!wrong_ttl_ack.has_value() && wrong_ttl_ack.error() == output_error::stale_context));
    CHECK((!wrong_foreground_ack.has_value() &&
           wrong_foreground_ack.error() == output_error::stale_context));
    REQUIRE(current.acknowledge(*retained).has_value());
    const auto duplicate = current.acknowledge(*retained);
    CHECK((!duplicate.has_value() && duplicate.error() == output_error::stale_context));
}

TEST_CASE("output.internal_foreign_incarnation_is_fenced_before_maintenance_and_ack",
    "[component][output]") {
    authority_fixture fixture{};
    inputnext::output::detail::composition foreign_root{fixture.runtime,
        fixture.registry,
        fixture.focus_observer};
    auto& foreign_owner = foreign_root.public_authority();
    const internal_context_options options{
        .owner = internal_owner_key{880U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::foreground_bound,
    };
    auto victim = inputnext::test::output::make_internal_producer(fixture.trusted, options);
    auto foreign = inputnext::test::output::make_internal_producer(foreign_root, options);
    auto focus_options = options;
    focus_options.owner = internal_owner_key{881U};
    auto victim_focus_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, focus_options);
    auto foreign_focus_producer =
        inputnext::test::output::make_internal_producer(foreign_root, focus_options);
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    inputnext::output::detail::authority_test_access::use_clock(foreign_owner, ticks);

    const auto victim_context = victim.open(fixture.connection.target);
    const auto foreign_context = foreign.open(fixture.connection.target);
    REQUIRE(victim_context.has_value());
    REQUIRE(foreign_context.has_value());
    CHECK(victim_context->identity.id == foreign_context->identity.id);
    CHECK(victim_context->identity.target == foreign_context->identity.target);
    CHECK(victim_context->identity.internal_owner == foreign_context->identity.internal_owner);
    CHECK(victim_context->identity.incarnation != foreign_context->identity.incarnation);

    const auto same_history_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.3F, .high = 0.7F});
    const auto victim_mutation = victim.submit(*victim_context, same_history_request);
    const auto foreign_mutation = foreign.submit(*foreign_context, same_history_request);
    REQUIRE(victim_mutation.has_value());
    REQUIRE(foreign_mutation.has_value());
    CHECK(victim_mutation->accepted_sequence == foreign_mutation->accepted_sequence);
    CHECK(victim_mutation->resulting_revisions == foreign_mutation->resulting_revisions);
    CHECK(victim_mutation->logical_version == foreign_mutation->logical_version);
    const auto victim_close = victim.close(*victim_context, victim_context->identity.id.value);
    const auto foreign_close = foreign.close(*foreign_context, foreign_context->identity.id.value);
    REQUIRE(victim_close.has_value());
    REQUIRE(foreign_close.has_value());
    CHECK(victim_close->close_request_id == foreign_close->close_request_id);
    CHECK(victim_close->logical_revocation_version == foreign_close->logical_revocation_version);
    auto normalized_foreign_receipt = *foreign_close;
    normalized_foreign_receipt.identity.incarnation = victim_close->identity.incarnation;
    CHECK(normalized_foreign_receipt == *victim_close);

    const auto foreign_receipt_at_victim = victim.acknowledge(*foreign_close);
    const auto victim_receipt_at_foreign = foreign.acknowledge(*victim_close);
    const auto public_ack_of_internal = fixture.owner.acknowledge(*victim_close);
    const auto public_context = fixture.owner.open(fixture.connection.target);
    const auto foreign_public_context = foreign_owner.open(fixture.connection.target);
    REQUIRE(public_context.has_value());
    REQUIRE(foreign_public_context.has_value());
    CHECK(public_context->identity.id == foreign_public_context->identity.id);
    const auto public_receipt =
        fixture.owner.close(*public_context, public_context->identity.id.value);
    const auto foreign_public_receipt =
        foreign_owner.close(*foreign_public_context, foreign_public_context->identity.id.value);
    REQUIRE(public_receipt.has_value());
    REQUIRE(foreign_public_receipt.has_value());
    const auto producer_ack_of_public = victim.acknowledge(*public_receipt);
    CHECK((!foreign_receipt_at_victim.has_value() &&
           foreign_receipt_at_victim.error() == output_error::stale_context));
    CHECK((!victim_receipt_at_foreign.has_value() &&
           victim_receipt_at_foreign.error() == output_error::stale_context));
    CHECK((!public_ack_of_internal.has_value() &&
           public_ack_of_internal.error() == output_error::stale_context));
    CHECK((!producer_ack_of_public.has_value() &&
           producer_ack_of_public.error() == output_error::stale_context));
    const auto retained_victim =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_context->identity);
    REQUIRE(retained_victim.has_value());
    const auto retained_victim_state = retained_victim.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(retained_victim_state.receipt == *victim_close);
    REQUIRE(victim.acknowledge(*victim_close).has_value());
    REQUIRE(foreign.acknowledge(*foreign_close).has_value());

    const auto victim_due = victim.open(fixture.connection.target);
    const auto foreign_due = foreign.open(fixture.connection.target);
    const auto victim_focus = victim_focus_producer.open(fixture.connection.target);
    const auto foreign_focus = foreign_focus_producer.open(fixture.connection.target);
    REQUIRE(victim_due.has_value());
    REQUIRE(foreign_due.has_value());
    REQUIRE(victim_focus.has_value());
    REQUIRE(foreign_focus.has_value());
    CHECK(victim_due->identity.id == foreign_due->identity.id);
    CHECK(victim_focus->identity.id == foreign_focus->identity.id);
    const auto due_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.5F, .high = 0.8F});
    const auto focus_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.2F, .high = 0.4F});
    const auto victim_due_receipt = victim.submit(*victim_due, due_request);
    const auto foreign_due_receipt = foreign.submit(*foreign_due, due_request);
    const auto victim_focus_receipt = victim_focus_producer.submit(*victim_focus, focus_request);
    const auto foreign_focus_receipt = foreign_focus_producer.submit(*foreign_focus, focus_request);
    REQUIRE(victim_due_receipt.has_value());
    REQUIRE(foreign_due_receipt.has_value());
    REQUIRE(victim_focus_receipt.has_value());
    REQUIRE(foreign_focus_receipt.has_value());

    ticks.store(std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms).count(),
        std::memory_order_release);
    fixture.focus_observer.stop();
    const auto before_version =
        inputnext::output::detail::authority_test_access::logical_version(fixture.owner);
    const auto due_before =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_due->identity);
    const auto focus_before =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_focus->identity);
    REQUIRE(due_before.has_value());
    REQUIRE(focus_before.has_value());
    const auto due_state_before =
        due_before.value_or(inputnext::output::detail::authority_test_access::record_observation{});
    const auto focus_state_before = focus_before.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(due_state_before.open);
    CHECK(due_state_before.active[2U]);
    CHECK(focus_state_before.open);
    CHECK(focus_state_before.active[2U]);
    CHECK(due_state_before.logical_version == before_version);
    CHECK(focus_state_before.logical_version == before_version);

    const auto foreign_submit_at_victim = victim.submit(*foreign_due,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.9F, .high = 0.95F}));
    const clear_request foreign_clear{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = foreign_due_receipt->resulting_revisions,
    };
    const auto foreign_clear_at_victim = victim.clear(*foreign_due, foreign_clear);
    const auto foreign_close_at_victim = victim.close(*foreign_due, foreign_due->identity.id.value);
    const auto foreign_focus_submit_at_victim = victim_focus_producer.submit(*foreign_focus,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.9F, .high = 0.95F}));
    CHECK((!foreign_submit_at_victim.has_value() &&
           foreign_submit_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_clear_at_victim.has_value() &&
           foreign_clear_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_close_at_victim.has_value() &&
           foreign_close_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_focus_submit_at_victim.has_value() &&
           foreign_focus_submit_at_victim.error() == output_error::stale_context));

    const auto due_after =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_due->identity);
    const auto focus_after =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_focus->identity);
    REQUIRE(due_after.has_value());
    REQUIRE(focus_after.has_value());
    const auto due_state_after =
        due_after.value_or(inputnext::output::detail::authority_test_access::record_observation{});
    const auto focus_state_after = focus_after.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(due_state_after.open);
    CHECK(due_state_after.accepted_sequence == due_state_before.accepted_sequence);
    CHECK(due_state_after.revisions == victim_due_receipt->resulting_revisions);
    CHECK(due_state_after.active == due_state_before.active);
    CHECK(due_state_after.logical_version == before_version);
    CHECK(focus_state_after.open);
    CHECK(focus_state_after.accepted_sequence == focus_state_before.accepted_sequence);
    CHECK(focus_state_after.revisions == victim_focus_receipt->resulting_revisions);
    CHECK(focus_state_after.active == focus_state_before.active);
    CHECK(focus_state_after.logical_version == before_version);
    CHECK(inputnext::output::detail::authority_test_access::logical_version(fixture.owner) ==
          before_version);
}

} // namespace
