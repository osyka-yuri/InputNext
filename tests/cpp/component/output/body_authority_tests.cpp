#include "values/required_value.hpp"
#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::native_values;
using inputnext::output::detail::project_representation;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

[[nodiscard]] body_request body_request_for(std::uint64_t sequence,
    std::chrono::milliseconds ttl = 1s,
    rumble_value fallback = {}) {
    return body_request{.sequence = sequence,
        .ttl = ttl,
        .preference = body_route_preference::classic,
        .fallback = fallback};
}

TEST_CASE("output.body_lease_is_exact_and_has_an_independent_sequence", "[component][output]") {
    authority_fixture fixture{};
    const auto first = fixture.owner.open(fixture.connection.target);
    const auto contender = fixture.owner.open(fixture.connection.target);
    REQUIRE(first.has_value());
    REQUIRE(contender.has_value());

    auto discrete = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 40U, .ttl = 5s, .low = 0.2F, .high = 0.3F});
    const auto discrete_receipt = fixture.owner.submit(*first, discrete);
    REQUIRE(discrete_receipt.has_value());

    const auto acquired = fixture.owner.submit_body(*first, body_request_for(1U));
    REQUIRE(acquired.has_value());
    CHECK(acquired->identity.context == first->identity);
    CHECK(acquired->identity.lease.valid());
    CHECK(acquired->accepted_sequence == 1U);

    const auto duplicate_owner = fixture.owner.submit_body(*first, body_request_for(1U));
    CHECK((!duplicate_owner.has_value() && duplicate_owner.error() == output_error::busy));
    const auto second_owner = fixture.owner.submit_body(*contender, body_request_for(1U));
    CHECK((!second_owner.has_value() && second_owner.error() == output_error::busy));

    auto renewed = body_request_for(2U, 2s, {.low_frequency = 0.0F, .high_frequency = 0.0F});
    renewed.lease = acquired->identity.lease;
    renewed.audio = acquired->identity.audio;
    const auto renewed_receipt = fixture.owner.submit_body(*first, renewed);
    REQUIRE(renewed_receipt.has_value());
    CHECK(renewed_receipt->accepted_sequence == 2U);

    discrete.sequence = 41U;
    discrete.values.rumble = {.low_frequency = 0.8F, .high_frequency = 0.7F};
    const auto latest_discrete = fixture.owner.submit(*first, discrete);
    REQUIRE(latest_discrete.has_value());

    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->present_channels() == rumble_mask);
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).fallback_rumble == rumble_value{});
    REQUIRE(snapshot->rumble.has_value());
    CHECK(required_value(snapshot->rumble).value == discrete.values.rumble);
    const auto projected = project_representation(*snapshot, rumble_mask);
    REQUIRE(projected.has_value());
    CHECK(native_values(*projected).values.rumble == rumble_value{});

    const body_clear_request clear{
        .lease = renewed_receipt->identity.lease,
        .audio = renewed_receipt->identity.audio,
        .sequence = 3U,
    };
    REQUIRE(fixture.owner.clear_body(*first, clear).has_value());
    const auto restored = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(restored.has_value());
    CHECK(!(restored->body.has_value()));
    REQUIRE(restored->rumble.has_value());
    CHECK(required_value(restored->rumble).value == discrete.values.rumble);

    const auto stale_renewal = fixture.owner.submit_body(*first, renewed);
    CHECK((!stale_renewal.has_value() && stale_renewal.error() == output_error::stale_lease));
}

TEST_CASE("output.body_clear_and_id_exhaustion_preserve_reserved_cleanup_headroom",
    "[component][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    inputnext::output::detail::authority_test_access::set_next_body_lease_id(fixture.owner,
        std::numeric_limits<std::uint64_t>::max());

    const auto final_lease = fixture.owner.submit_body(*opened, body_request_for(1U));
    REQUIRE(final_lease.has_value());
    CHECK(final_lease->identity.lease.value == std::numeric_limits<std::uint64_t>::max());
    inputnext::output::detail::authority_test_access::set_logical_version(fixture.owner,
        std::numeric_limits<std::uint64_t>::max() - 2U);

    const body_clear_request clear{.lease = final_lease->identity.lease,
        .audio = final_lease->identity.audio,
        .sequence = 2U};
    REQUIRE(fixture.owner.clear_body(*opened, clear).has_value());
    CHECK(inputnext::output::detail::authority_test_access::logical_version(fixture.owner) ==
          std::numeric_limits<std::uint64_t>::max() - 1U);

    inputnext::output::detail::authority_test_access::set_logical_version(fixture.owner, 0U);
    const auto exhausted = fixture.owner.submit_body(*opened, body_request_for(1U));
    CHECK((!exhausted.has_value() && exhausted.error() == output_error::identity_exhausted));
    const auto closed = fixture.owner.close(*opened, opened->identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(closed->logical_revocation_version == 1U);
}

} // namespace
