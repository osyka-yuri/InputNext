#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

TEST_CASE("output.authority_expiry_preserves_provenance_and_uses_entry_time",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto accepted = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.4F, .high = 0.6F}));
    REQUIRE(accepted.has_value());

    using clock_duration = std::chrono::steady_clock::duration;
    const auto deadline_ticks = std::chrono::duration_cast<clock_duration>(1ms).count();
    ticks.store(static_cast<std::int64_t>(deadline_ticks), std::memory_order_release);
    const auto expired = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(expired.has_value());
    CHECK(expired->present_channels() == 0U);
    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 1U);
    CHECK(info->revisions[2U] == 1U);

    ticks.store(0, std::memory_order_release);
    const auto delayed = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 1ms, .low = 0.8F, .high = 0.9F}));
    CHECK((!delayed.has_value() && delayed.error() == output_error::expired));
    const auto after = fixture.owner.inspect(*opened);
    REQUIRE(after.has_value());
    CHECK(after->accepted_sequence == 1U);
    CHECK(after->revisions[2U] == 1U);
}

TEST_CASE("output.snapshot_earliest_expiry_includes_nonwinning_active_channels",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const internal_context_options winner_options{
        .owner = internal_owner_key{81U},
        .priority = internal_priority::diagnostic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    const internal_context_options lower_options{
        .owner = internal_owner_key{82U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto winner_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, winner_options);
    auto lower_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, lower_options);
    const auto winner = winner_producer.open(fixture.connection.target);
    const auto lower = lower_producer.open(fixture.connection.target);
    REQUIRE(winner.has_value());
    REQUIRE(lower.has_value());
    const auto winner_submit = winner_producer.submit(*winner,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.75F, .high = 0.75F}));
    REQUIRE(winner_submit.has_value());
    const auto lower_submit = lower_producer.submit(*lower,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.25F, .high = 0.25F}));
    REQUIRE(lower_submit.has_value());

    const auto before_expiry = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(before_expiry.has_value());
    REQUIRE(before_expiry->rumble.has_value());
    CHECK(before_expiry->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          winner->identity);
    CHECK(before_expiry->earliest_expiry ==
          std::chrono::steady_clock::time_point{
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms)});

    ticks.store(std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms).count(),
        std::memory_order_release);
    const auto at_expiry = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(at_expiry.has_value());
    REQUIRE(at_expiry->rumble.has_value());
    CHECK(at_expiry->rumble.value_or(channel_snapshot<rumble_value>{}).provenance.identity ==
          winner->identity);
    CHECK(at_expiry->earliest_expiry ==
          std::chrono::steady_clock::time_point{
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(5s)});
}

TEST_CASE("output.submit_commit_rechecks_original_deadline_after_registry_wait",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto first = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.2F, .high = 0.3F}));
    REQUIRE(first.has_value());

    const auto entry_time = std::chrono::steady_clock::time_point{};
    const auto second_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 2U, .ttl = 1ms, .low = 0.8F, .high = 0.9F});
    const auto original_deadline =
        policy_detail::make_deadline(entry_time, second_request.ttl, ttl_permission::finite_only);
    REQUIRE(original_deadline.has_value());
    REQUIRE(original_deadline->has_value());

    auto gate = inputnext::output::detail::authority_test_access::lock_gate(fixture.owner);
    auto captured = fixture.registry.capture(fixture.connection.target);
    REQUIRE(captured.has_value());
    const auto deadline = original_deadline->value_or(std::chrono::steady_clock::time_point{});
    ticks.store(deadline.time_since_epoch().count(), std::memory_order_release);
    captured->release();
    const auto rejected =
        inputnext::output::detail::authority_test_access::commit_submit_locked(fixture.owner,
            opened->identity,
            second_request,
            *original_deadline,
            entry_time);
    CHECK((!rejected.has_value() && rejected.error() == output_error::expired));
    gate.unlock();

    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 1U);
    CHECK(info->revisions[2U] == 1U);
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    CHECK((snapshot->present_channels() & rumble_mask) == 0U);
}

TEST_CASE("output.snapshot_projection_rechecks_time_after_registry_capture",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto accepted = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.4F, .high = 0.6F}));
    REQUIRE(accepted.has_value());
    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    REQUIRE(info->foreground.has_value());
    const auto entry_time = std::chrono::steady_clock::time_point{};
    const auto deadline_ticks =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms);
    const auto deadline = std::chrono::steady_clock::time_point{deadline_ticks};

    auto gate = inputnext::output::detail::authority_test_access::lock_gate(fixture.owner);
    auto captured = fixture.registry.capture(fixture.connection.target);
    REQUIRE(captured.has_value());
    ticks.store(deadline_ticks.count(), std::memory_order_release);
    const auto epoch = info->foreground.value_or(focus::foreground_epoch{});
    const auto snapshot =
        inputnext::output::detail::authority_test_access::finish_snapshot_locked(fixture.owner,
            fixture.connection.target,
            entry_time,
            epoch,
            epoch,
            std::move(*captured));
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->evaluated_at == deadline);
    CHECK(snapshot->present_channels() == 0U);
    CHECK(snapshot->logical_version > accepted->logical_version);
    gate.unlock();

    const auto final_info = fixture.owner.inspect(*opened);
    REQUIRE(final_info.has_value());
    CHECK(final_info->accepted_sequence == 1U);
    CHECK(final_info->revisions[2U] == 1U);
}

} // namespace
