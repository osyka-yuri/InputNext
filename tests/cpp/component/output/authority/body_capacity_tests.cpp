#include "output/authority/execution.hpp"
#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/projection/represented.hpp"
#include "values/required_value.hpp"
#include <inputnext/device_registry.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::authority_execution_access;
using inputnext::output::detail::authority_test_access;
using inputnext::output::detail::completion_disposition;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::registry_source;

TEST_CASE("output.body_audio_capacity_recovers_after_observed_connection_retirement",
    "[component][output]") {
    constexpr std::size_t audio_capacity = 16U;
    authority_fixture fixture;
    auto control = fixture.trusted.audio_routes();
    std::array<audio_instance, audio_capacity> instances{};
    for (std::size_t index = 0U; index < audio_capacity; ++index) {
        auto target = fixture.connection.target;
        if (index != 0U) {
            std::unique_ptr<inputnext::source_lifetime> source =
                std::make_unique<registry_source>();
            const auto admitted = fixture.registry.admit(source);
            REQUIRE(admitted.has_value());
            target = admitted->target;
        }
        const auto attached = control.attach(target, body_route_support::classic_only);
        REQUIRE(attached.has_value());
        instances[index] = *attached;
    }
    CHECK(fixture.registry.live_count() == audio_capacity);
    REQUIRE(fixture.registry.retire(instances.front().target).has_value());
    std::unique_ptr<inputnext::source_lifetime> source = std::make_unique<registry_source>();
    const auto replacement = fixture.registry.admit(source);
    REQUIRE(replacement.has_value());
    const auto attached = control.attach(replacement->target, body_route_support::classic_only);
    REQUIRE(attached.has_value());
    CHECK(attached->id != instances.front().id);
    const auto old = control.detach(instances.front());
    REQUIRE_FALSE(old.has_value());
    CHECK(old.error() == error::stale_audio);
    REQUIRE(control.set_ready(*attached, true).has_value());
    REQUIRE(control.detach(*attached).has_value());
    for (std::size_t index = 1U; index < audio_capacity; ++index) {
        CHECK(control.detach(instances[index]).has_value());
    }
}

TEST_CASE("output.body_hd_cleanup_retains_headroom_at_the_logical_version_limit",
    "[component][output]") {
    std::atomic<std::int64_t> const ticks{0};
    authority_fixture fixture;
    authority_test_access::use_clock(fixture.owner, ticks);
    auto control = fixture.trusted.audio_routes();
    const auto audio = control.attach(fixture.connection.target, body_route_support::hd, true);
    REQUIRE(audio.has_value());
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto body = fixture.owner.submit_body(*context,
        body_request{.audio = *audio,
            .sequence = 1U,
            .ttl = 5s,
            .preference = body_route_preference::prefer_hd});
    REQUIRE(body.has_value());
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    const auto projection =
        detail::project_representation(*snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(projection.has_value());
    const auto route = detail::represented_body_route(*projection);
    REQUIRE(route.has_value());
    // One context, one body, its HD invalidation, one attachment and its readiness loss.
    constexpr std::uint64_t reserved_transitions = 5U;
    const auto initial_version = std::numeric_limits<std::uint64_t>::max() - reserved_transitions;
    authority_test_access::set_logical_version(fixture.owner, initial_version);
    bool attached = true;

    SECTION("native failure") {
        const auto execution = authority_execution_access::attach(fixture.owner);
        REQUIRE(execution.has_value());
        const auto completed = authority_execution_access::complete_route(fixture.owner,
            *execution,
            required_value(route),
            native_status::failed);
        CHECK(completed.has_value());
        if (completed.has_value()) {
            CHECK(*completed == completion_disposition::fallback_committed);
        }
        CHECK(authority_execution_access::detach(fixture.owner, *execution).has_value());
    }
    SECTION("readiness loss") {
        REQUIRE(control.set_ready(*audio, false).has_value());
    }
    SECTION("detach") {
        REQUIRE(control.detach(*audio).has_value());
        attached = false;
    }

    const auto fallback = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(fallback.has_value());
    REQUIRE(fallback->body.has_value());
    CHECK(required_value(fallback->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(fallback->body).intent.identity == body->identity);
    CHECK(fallback->logical_version > initial_version);
    if (attached) {
        REQUIRE(control.set_ready(*audio, false).has_value());
        REQUIRE(control.detach(*audio).has_value());
    }
    REQUIRE(fixture.owner
            .clear_body(*context,
                body_clear_request{.lease = body->identity.lease, .audio = *audio, .sequence = 2U})
            .has_value());
    const auto closed = fixture.owner.close(*context, context->identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(fixture.owner.acknowledge(*closed).has_value());
}

} // namespace
