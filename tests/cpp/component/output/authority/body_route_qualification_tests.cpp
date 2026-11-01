#include "output/authority/execution.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::authority_execution_access;
using inputnext::output::detail::authority_test_access;
using inputnext::test::output::authority_fixture;

TEST_CASE("output.body_route_qualification_rejects_foreign_owner_before_expiry_maintenance",
    "[component][output][execution][body_route]") {
    authority_fixture fixture;
    std::atomic<std::int64_t> ticks{};
    authority_test_access::use_clock(fixture.owner, ticks);

    const auto audio = fixture.trusted.audio_routes().attach(fixture.connection.target,
        body_route_support::hd,
        true);
    REQUIRE(audio.has_value());
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto body = fixture.owner.submit_body(*context,
        body_request{.audio = *audio,
            .sequence = 1U,
            .ttl = 10ms,
            .preference = body_route_preference::prefer_hd});
    REQUIRE(body.has_value());

    const auto attached = authority_execution_access::attach(fixture.owner);
    REQUIRE(attached.has_value());
    auto foreign = *attached;
    foreign.id.value += 1U;

    using clock = std::chrono::steady_clock;
    ticks.store(std::chrono::duration_cast<clock::duration>(20ms).count(),
        std::memory_order_release);
    const auto version_before = authority_test_access::logical_version(fixture.owner);

    const auto rejected =
        authority_execution_access::qualifies_route(fixture.owner, foreign, body->intent);
    CHECK((!rejected.has_value() && rejected.error() == execution_error::stale_backend));
    const auto rejected_completion = authority_execution_access::complete_route(fixture.owner,
        foreign,
        body->intent,
        native_status::failed);
    CHECK((!rejected_completion.has_value() &&
           rejected_completion.error() == execution_error::stale_backend));
    CHECK(authority_test_access::logical_version(fixture.owner) == version_before);

    auto invalid = body->intent;
    invalid.identity.context.id = {};
    const auto invalid_rejected =
        authority_execution_access::qualifies_route(fixture.owner, *attached, invalid);
    CHECK((!invalid_rejected.has_value() &&
           invalid_rejected.error() == execution_error::invalid_argument));
    const auto invalid_completion = authority_execution_access::complete_route(fixture.owner,
        *attached,
        invalid,
        native_status::failed);
    CHECK((!invalid_completion.has_value() &&
           invalid_completion.error() == execution_error::invalid_argument));
    CHECK(authority_test_access::logical_version(fixture.owner) == version_before);

    const auto expired =
        authority_execution_access::qualifies_route(fixture.owner, *attached, body->intent);
    REQUIRE(expired.has_value());
    CHECK(!*expired);
    CHECK(authority_test_access::logical_version(fixture.owner) > version_before);

    CHECK(authority_execution_access::detach(fixture.owner, *attached).has_value());
}

} // namespace
