#include "output/authority/execution.hpp"

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::output::detail::authority_execution_access;
using inputnext::output::detail::authority_test_access;
using inputnext::output::detail::internal_producer;
using inputnext::output::detail::project_representation;
using inputnext::test::output::authority_fixture;

[[nodiscard]] internal_producer make_background_producer(authority_fixture& fixture,
    const std::uint64_t owner_key) {
    const internal_context_options options{.owner = internal_owner_key{owner_key},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background};
    return inputnext::test::output::make_internal_producer(fixture.trusted, options);
}

TEST_CASE("output.execution_qualification_rejects_foreign_owner_before_ttl_maintenance",
    "[component][output]") {
    authority_fixture fixture{};
    authority peer{fixture.runtime, fixture.registry, fixture.focus_observer};
    std::atomic<std::int64_t> clock_ticks{0};
    authority_test_access::use_clock(fixture.owner, clock_ticks);

    auto producer = make_background_producer(fixture, 301U);
    const auto context = producer.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto submitted = producer.submit(*context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5ms, .low = 0.3F, .high = 0.6F}));
    REQUIRE(submitted.has_value());

    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    const auto expected = project_representation(*snapshot, rumble_mask);
    REQUIRE(expected.has_value());
    const auto execution = authority_execution_access::attach(peer);
    const auto correct_execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    REQUIRE(correct_execution.has_value());

    const auto version_before = authority_test_access::logical_version(fixture.owner);
    clock_ticks.store(6'000'000, std::memory_order_release);
    const auto rejected =
        authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == execution_error::stale_backend);
    CHECK(authority_test_access::logical_version(fixture.owner) == version_before);

    const auto retained = authority_test_access::observe_record(fixture.owner, context->identity);
    REQUIRE(retained.has_value());
    const auto retained_state = retained.value_or(authority_test_access::record_observation{});
    CHECK(retained_state.open);

    REQUIRE(authority_execution_access::detach(fixture.owner, *correct_execution).has_value());
    REQUIRE(authority_execution_access::detach(peer, *execution).has_value());
}

TEST_CASE("output.execution_qualification_tracks_represented_provenance_not_global_version",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> const clock_ticks{0};
    authority_test_access::use_clock(fixture.owner, clock_ticks);
    auto producer = make_background_producer(fixture, 302U);

    const auto first_context = producer.open(fixture.connection.target);
    REQUIRE(first_context.has_value());
    const auto first_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 100ms, .low = 0.3F, .high = 0.6F});
    REQUIRE(producer.submit(*first_context, first_request).has_value());

    const auto first_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(first_snapshot.has_value());
    const auto first_projection = project_representation(*first_snapshot, rumble_mask);
    REQUIRE(first_projection.has_value());
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    const auto first_is_current =
        authority_execution_access::qualifies(fixture.owner, *execution, *first_projection);
    REQUIRE(first_is_current.has_value());
    CHECK(*first_is_current);

    auto repeated_request = first_request;
    repeated_request.sequence = 2U;
    REQUIRE(producer.submit(*first_context, repeated_request).has_value());
    const auto first_is_stale =
        authority_execution_access::qualifies(fixture.owner, *execution, *first_projection);
    REQUIRE(first_is_stale.has_value());
    CHECK_FALSE(*first_is_stale);

    auto candidate = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto second_connection = fixture.registry.admit(candidate);
    REQUIRE(second_connection.has_value());
    const auto second_context = producer.open(second_connection->target);
    REQUIRE(second_context.has_value());
    const auto second_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 100ms, .low = 0.8F, .high = 0.2F});
    REQUIRE(producer.submit(*second_context, second_request).has_value());

    const auto latest_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(latest_snapshot.has_value());
    const auto latest_projection = project_representation(*latest_snapshot, rumble_mask);
    REQUIRE(latest_projection.has_value());
    REQUIRE(latest_projection->rumble.has_value());
    const auto latest_rumble = latest_projection->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(latest_rumble.provenance.accepted_sequence == 2U);
    CHECK(latest_snapshot->logical_version > first_snapshot->logical_version);
    const auto latest_is_current =
        authority_execution_access::qualifies(fixture.owner, *execution, *latest_projection);
    REQUIRE(latest_is_current.has_value());
    CHECK(*latest_is_current);

    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
}

TEST_CASE("output.execution_qualification_revokes_a_projection_after_foreground_loss",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> const clock_ticks{0};
    authority_test_access::use_clock(fixture.owner, clock_ticks);

    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 100ms, .low = 0.5F, .high = 0.4F});
    REQUIRE(fixture.owner.submit(*context, request).has_value());

    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->rumble.has_value());
    const auto expected = project_representation(*snapshot, rumble_mask);
    REQUIRE(expected.has_value());
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());

    fixture.focus_observer.stop();
    const auto current =
        authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE(current.has_value());
    CHECK_FALSE(*current);

    const auto revoked = authority_test_access::observe_record(fixture.owner, context->identity);
    REQUIRE(revoked.has_value());
    const auto revoked_state = revoked.value_or(authority_test_access::record_observation{});
    CHECK_FALSE(revoked_state.open);
    REQUIRE(revoked_state.receipt.has_value());
    const auto receipt = revoked_state.receipt.value_or(close_receipt{});
    CHECK(receipt.reason == close_reason::foreground_revocation);

    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
}

TEST_CASE("output.execution_qualification_observes_exact_connection_retirement",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> const clock_ticks{0};
    authority_test_access::use_clock(fixture.owner, clock_ticks);
    auto producer = make_background_producer(fixture, 303U);

    const auto context = producer.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 100ms, .low = 0.7F, .high = 0.2F});
    REQUIRE(producer.submit(*context, request).has_value());
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    const auto expected = project_representation(*snapshot, rumble_mask);
    REQUIRE(expected.has_value());
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    const auto current =
        authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE(current.has_value());
    CHECK(*current);

    REQUIRE(fixture.registry.retire(fixture.connection.target).has_value());
    const auto retired =
        authority_execution_access::qualifies(fixture.owner, *execution, *expected);
    REQUIRE_FALSE(retired.has_value());
    CHECK(retired.error() == execution_error::not_connected);

    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
}

TEST_CASE("output.execution_registration_checks_the_exact_live_connection", "[component][output]") {
    authority_fixture fixture{};
    const auto execution = authority_execution_access::attach(fixture.owner);
    REQUIRE(execution.has_value());
    REQUIRE(authority_execution_access::check_target(fixture.owner,
        *execution,
        fixture.connection.target)
            .has_value());

    auto candidate = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto replacement = fixture.registry.replace(fixture.connection.target, candidate);
    REQUIRE(replacement.has_value());

    const auto stale = authority_execution_access::check_target(fixture.owner,
        *execution,
        fixture.connection.target);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == execution_error::not_connected);
    REQUIRE(authority_execution_access::check_target(fixture.owner, *execution, replacement->target)
            .has_value());

    REQUIRE(authority_execution_access::detach(fixture.owner, *execution).has_value());
}

} // namespace
