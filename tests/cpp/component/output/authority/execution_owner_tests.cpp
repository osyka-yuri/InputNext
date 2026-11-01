#include "output/authority/execution.hpp"

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::output::detail::authority_execution_access;
using inputnext::output::detail::authority_test_access;
using inputnext::test::output::authority_fixture;

TEST_CASE("output.execution_owner_attachment_is_exact_and_monotonic", "[component][output]") {
    authority_fixture fixture{};
    const auto attached = authority_execution_access::attach(fixture.owner);
    REQUIRE(attached.has_value());
    const auto owner = *attached;

    CHECK(owner.valid());
    CHECK(owner.authority == authority_test_access::incarnation(fixture.owner));
    CHECK(owner.id == execution_id{1U});
    CHECK(authority_test_access::attached_execution(fixture.owner) == owner);

    const auto duplicate = authority_execution_access::attach(fixture.owner);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == execution_error::already_attached);
    CHECK(authority_test_access::next_execution_id(fixture.owner) == 2U);

    auto foreign_owner = owner;
    ++foreign_owner.id.value;
    const auto foreign_detach = authority_execution_access::detach(fixture.owner, foreign_owner);
    REQUIRE_FALSE(foreign_detach.has_value());
    CHECK(foreign_detach.error() == execution_error::stale_backend);
    CHECK(authority_test_access::attached_execution(fixture.owner) == owner);

    REQUIRE(authority_execution_access::detach(fixture.owner, owner).has_value());
    const auto repeated_detach = authority_execution_access::detach(fixture.owner, owner);
    REQUIRE_FALSE(repeated_detach.has_value());
    CHECK(repeated_detach.error() == execution_error::not_running);

    const auto next = authority_execution_access::attach(fixture.owner);
    REQUIRE(next.has_value());
    CHECK(next->id == execution_id{2U});
    CHECK(next->authority == owner.authority);
    REQUIRE(authority_execution_access::detach(fixture.owner, *next).has_value());
}

TEST_CASE("output.execution_owners_are_distinct_across_authorities_sharing_dependencies",
    "[component][output]") {
    authority_fixture fixture{};
    authority peer{fixture.runtime, fixture.registry, fixture.focus_observer};

    const auto original = authority_execution_access::attach(fixture.owner);
    const auto independent = authority_execution_access::attach(peer);
    REQUIRE(original.has_value());
    REQUIRE(independent.has_value());

    CHECK(original->id == independent->id);
    CHECK(original->authority != independent->authority);
    CHECK(*original != *independent);

    REQUIRE(authority_execution_access::detach(peer, *independent).has_value());
    REQUIRE(authority_execution_access::detach(fixture.owner, *original).has_value());
}

TEST_CASE("output.execution_owner_identity_exhaustion_never_wraps", "[component][output]") {
    authority_fixture fixture{};
    authority_test_access::set_next_execution_id(fixture.owner,
        std::numeric_limits<std::uint64_t>::max());

    const auto last = authority_execution_access::attach(fixture.owner);
    REQUIRE(last.has_value());
    CHECK(last->id.value == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(authority_execution_access::detach(fixture.owner, *last).has_value());

    const auto exhausted = authority_execution_access::attach(fixture.owner);
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error() == execution_error::identity_exhausted);
    CHECK(authority_test_access::next_execution_id(fixture.owner) == 0U);
}

} // namespace
