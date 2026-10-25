#include "values/required_value.hpp"
#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

[[nodiscard]] body_request request_for(std::uint64_t sequence, std::chrono::milliseconds ttl = 5s) {
    return body_request{.sequence = sequence, .ttl = ttl};
}

TEST_CASE("output.body_expiry_is_inclusive_and_revokes_the_lease", "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{0};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto acquired = fixture.owner.submit_body(*owner, request_for(1U, 10ms));
    REQUIRE(acquired.has_value());

    using clock = std::chrono::steady_clock;
    ticks.store(std::chrono::duration_cast<clock::duration>(10ms).count(),
        std::memory_order_release);
    const auto expired = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(expired.has_value());
    CHECK_FALSE(expired->body.has_value());
    CHECK((expired->present_channels() & rumble_mask) == 0U);

    const body_clear_request stale_clear{.lease = acquired->identity.lease,
        .audio = acquired->identity.audio,
        .sequence = 2U};
    const auto stale = fixture.owner.clear_body(*owner, stale_clear);
    CHECK((!stale.has_value() && stale.error() == output_error::stale_lease));
}

TEST_CASE("output.body_deadline_uses_the_captured_call_entry_time", "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{0};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    inputnext::output::detail::authority_test_access::set_next_body_lease_id(fixture.owner, 17U);

    using clock = std::chrono::steady_clock;
    const auto entry_time = clock::time_point{};
    ticks.store(std::chrono::duration_cast<clock::duration>(10ms).count(),
        std::memory_order_release);
    const auto version_before =
        inputnext::output::detail::authority_test_access::logical_version(fixture.owner);
    const auto expired =
        inputnext::output::detail::authority_test_access::commit_body_locked(fixture.owner,
            owner->identity,
            request_for(1U, 10ms),
            entry_time);
    CHECK((!expired.has_value() && expired.error() == output_error::expired));
    const auto record =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            owner->identity);
    REQUIRE(record.has_value());
    CHECK_FALSE(required_value(record).body.has_value());
    CHECK(required_value(record).accepted_sequence == 0U);
    CHECK(
        inputnext::output::detail::authority_test_access::next_body_lease_id(fixture.owner) == 17U);
    CHECK(inputnext::output::detail::authority_test_access::logical_version(fixture.owner) ==
          version_before);
}

TEST_CASE("output.body_renewal_cannot_revive_a_lease_expired_during_qualification",
    "[component][output]") {
    authority_fixture fixture{};
    std::atomic<std::int64_t> ticks{0};
    using access = inputnext::output::detail::authority_test_access;
    access::use_clock(fixture.owner, ticks);
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto acquired = fixture.owner.submit_body(*owner, request_for(1U, 10ms));
    REQUIRE(acquired.has_value());
    const auto next_id = access::next_body_lease_id(fixture.owner);
    using clock = std::chrono::steady_clock;
    const auto entered_at = clock::time_point{} + 5ms;
    auto renewal = request_for(2U, 1s);
    renewal.lease = acquired->identity.lease;
    ticks.store(std::chrono::duration_cast<clock::duration>(10ms).count(),
        std::memory_order_release);
    const auto rejected =
        access::commit_body_locked(fixture.owner, owner->identity, renewal, entered_at);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == output_error::stale_lease);
    const auto stored = access::observe_record(fixture.owner, owner->identity);
    REQUIRE(stored.has_value());
    CHECK_FALSE(required_value(stored).body.has_value());
    CHECK(access::next_body_lease_id(fixture.owner) == next_id);
    const auto fresh = fixture.owner.submit_body(*owner, request_for(1U));
    REQUIRE(fresh.has_value());
    CHECK(fresh->identity.lease != acquired->identity.lease);
}

TEST_CASE("output.body_context_close_and_focus_revocation_remove_body_eligibility",
    "[component][output]") {
    authority_fixture fixture{};
    const auto closed_owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(closed_owner.has_value());
    const auto closed_body = fixture.owner.submit_body(*closed_owner, request_for(1U));
    REQUIRE(closed_body.has_value());
    const auto close = fixture.owner.close(*closed_owner, closed_owner->identity.id.value);
    REQUIRE(close.has_value());
    const auto after_close = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_close.has_value());
    CHECK_FALSE(after_close->body.has_value());
    const body_clear_request closed_clear{.lease = closed_body->identity.lease,
        .audio = closed_body->identity.audio,
        .sequence = 2U};
    const auto revoked = fixture.owner.clear_body(*closed_owner, closed_clear);
    CHECK((!revoked.has_value() && revoked.error() == output_error::revoked));
    REQUIRE(fixture.owner.acknowledge(*close).has_value());

    const auto focused_owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(focused_owner.has_value());
    const auto focused_body = fixture.owner.submit_body(*focused_owner, request_for(1U));
    REQUIRE(focused_body.has_value());
    const auto info = fixture.owner.inspect(*focused_owner);
    REQUIRE(info.has_value());
    REQUIRE(info->foreground.has_value());
    auto stale_epoch = required_value(info->foreground);
    ++stale_epoch.revision;
    REQUIRE(inputnext::output::detail::authority_test_access::set_opening_epoch(fixture.owner,
        focused_owner->identity,
        stale_epoch));

    const auto after_focus_loss = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_focus_loss.has_value());
    CHECK_FALSE(after_focus_loss->body.has_value());
    const body_clear_request focus_clear{.lease = focused_body->identity.lease,
        .audio = focused_body->identity.audio,
        .sequence = 2U};
    const auto focus_revoked = fixture.owner.clear_body(*focused_owner, focus_clear);
    CHECK((!focus_revoked.has_value() && focus_revoked.error() == output_error::revoked));
}

TEST_CASE("output.body_foreign_audio_is_rejected_before_target_maintenance",
    "[component][output]") {
    authority_fixture fixture{};
    authority_fixture foreign_fixture{};
    std::atomic<std::int64_t> ticks{0};
    using access = inputnext::output::detail::authority_test_access;
    access::use_clock(fixture.owner, ticks);
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    const auto acquired = fixture.owner.submit_body(*owner, request_for(1U, 10ms));
    REQUIRE(acquired.has_value());
    auto foreign_routes = foreign_fixture.trusted.audio_routes();
    const auto foreign_audio =
        foreign_routes.attach(foreign_fixture.connection.target, body_route_support::hd, true);
    REQUIRE(foreign_audio.has_value());
    auto supplied_audio = *foreign_audio;
    supplied_audio.target = fixture.connection.target;

    using clock = std::chrono::steady_clock;
    ticks.store(std::chrono::duration_cast<clock::duration>(10ms).count(),
        std::memory_order_release);
    const auto version_before = access::logical_version(fixture.owner);

    SECTION("submit") {
        auto request = request_for(2U);
        request.lease = acquired->identity.lease;
        request.audio = supplied_audio;
        const auto rejected = fixture.owner.submit_body(*owner, request);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == output_error::stale_audio);
    }
    SECTION("clear") {
        const auto rejected = fixture.owner.clear_body(*owner,
            body_clear_request{.lease = acquired->identity.lease,
                .audio = supplied_audio,
                .sequence = 2U});
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == output_error::stale_audio);
    }

    const auto stored = access::observe_record(fixture.owner, owner->identity);
    REQUIRE(stored.has_value());
    CHECK(required_value(stored).body.has_value());
    CHECK(access::logical_version(fixture.owner) == version_before);
    const auto maintained = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(maintained.has_value());
    CHECK_FALSE(maintained->body.has_value());
}

TEST_CASE("output.body_connection_retirement_removes_audio_and_lease_identity",
    "[component][output]") {
    authority_fixture fixture{};
    auto audio_routes = fixture.trusted.audio_routes();
    const auto audio = audio_routes.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(audio.has_value());
    const auto owner = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner.has_value());
    body_request const request{.audio = *audio,
        .sequence = 1U,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd};
    const auto body = fixture.owner.submit_body(*owner, request);
    REQUIRE(body.has_value());
    REQUIRE(fixture.registry.retire(fixture.connection.target).has_value());

    const auto unavailable = fixture.owner.read_snapshot(fixture.connection.target);
    CHECK((!unavailable.has_value() && unavailable.error() == output_error::not_connected));
    const auto stale_audio = audio_routes.detach(*audio);
    CHECK((!stale_audio.has_value() && stale_audio.error() == output_error::stale_audio));
    const auto stale_context = fixture.owner.inspect(*owner);
    CHECK((!stale_context.has_value() && stale_context.error() == output_error::stale_context));
}

} // namespace
