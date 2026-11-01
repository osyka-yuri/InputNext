#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"
#include "values/required_value.hpp"

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::authority_test_access;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;
using inputnext::test::output::registry_source;

TEST_CASE("output.executor_observation_qualifies_published_projection_and_rejects_foreign_backend",
    "[component][output][execution][observation]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> sink =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    const auto empty = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(empty.has_value());
    REQUIRE(scope.engine().publish(*backend, *empty).has_value());
    {
        const auto empty_observation = scope.engine().observe(*backend);
        REQUIRE(empty_observation.has_value());
        CHECK(empty_observation->projection_current);
        CHECK(empty_observation->observed_logical_version == empty->logical_version);
    }

    const auto rumble = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.7F}));
    REQUIRE(rumble.has_value());
    const auto rumble_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(rumble_snapshot.has_value());
    {
        const auto unpublished = scope.engine().observe(*backend);
        REQUIRE(unpublished.has_value());
        CHECK_FALSE(unpublished->projection_current);
        CHECK(unpublished->observed_logical_version <
              unpublished->authority_snapshot.logical_version);
    }

    {
        REQUIRE(scope.engine().publish(*backend, *rumble_snapshot).has_value());
        const auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(scope.engine().drive_one(*lane));
        const auto completed = scope.engine().observe(*backend);
        REQUIRE(completed.has_value());
        CHECK(completed->projection_current);
        CHECK(completed->progress.delivery == delivery_state::setter_accepted);
        const auto completed_attempt =
            inputnext::test::required_value(completed->progress.last_completed_attempt);
        CHECK(completed_attempt.result.status == native_status::accepted);
    }

    auto foreign_backend = *backend;
    ++foreign_backend.incarnation.id.value;
    const auto stale = scope.engine().observe(foreign_backend);
    CHECK((!stale.has_value() && stale.error() == execution_error::stale_backend));
}

TEST_CASE("output.executor_observation_remains_current_across_other_target_changes",
    "[component][output][execution][observation]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> sink =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    const auto initial = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(initial.has_value());
    REQUIRE(scope.engine().publish(*backend, *initial).has_value());
    publication_id prior_publication{};
    {
        const auto rumble = fixture.owner.submit(*opened,
            inputnext::test::output::rumble_request(
                {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.7F}));
        REQUIRE(rumble.has_value());
        const auto rumble_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(rumble_snapshot.has_value());
        REQUIRE(scope.engine().publish(*backend, *rumble_snapshot).has_value());
        const auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(scope.engine().drive_one(*lane));
        const auto completed = scope.engine().observe(*backend);
        REQUIRE(completed.has_value());
        CHECK(completed->projection_current);
        CHECK(completed->progress.delivery == delivery_state::setter_accepted);
        prior_publication = completed->progress.observed_publication;
    }

    {
        auto other_source =
            std::unique_ptr<inputnext::source_lifetime>{std::make_unique<registry_source>()};
        const auto other_connection = fixture.registry.admit(other_source);
        REQUIRE(other_connection.has_value());
        const auto other_context = fixture.owner.open(other_connection->target);
        REQUIRE(other_context.has_value());
        const auto other_mutation = fixture.owner.submit(*other_context,
            inputnext::test::output::rumble_request(
                {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.6F, .high = 0.3F}));
        REQUIRE(other_mutation.has_value());
    }
    {
        const auto after_other_target_mutation = scope.engine().observe(*backend);
        REQUIRE(after_other_target_mutation.has_value());
        CHECK(after_other_target_mutation->projection_current);
        CHECK(after_other_target_mutation->observed_logical_version <
              after_other_target_mutation->authority_snapshot.logical_version);
        CHECK(after_other_target_mutation->progress.observed_publication == prior_publication);
    }
}

TEST_CASE("output.executor_observation_preserves_equal_projection_and_rejects_old_versions",
    "[component][output][execution][observation]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> sink =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    const auto initial = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(initial.has_value());
    REQUIRE(scope.engine().publish(*backend, *initial).has_value());
    publication_id prior_publication{};
    {
        const auto rumble = fixture.owner.submit(*opened,
            inputnext::test::output::rumble_request(
                {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.7F}));
        REQUIRE(rumble.has_value());
        const auto rumble_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(rumble_snapshot.has_value());
        REQUIRE(scope.engine().publish(*backend, *rumble_snapshot).has_value());
        const auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(scope.engine().drive_one(*lane));
        const auto completed = scope.engine().observe(*backend);
        REQUIRE(completed.has_value());
        CHECK(completed->projection_current);
        CHECK(completed->progress.delivery == delivery_state::setter_accepted);
        prior_publication = completed->progress.observed_publication;
    }

    submit_request unsupported_update{};
    unsupported_update.selected_channels = leds_mask;
    unsupported_update.sequence = 2U;
    unsupported_update.ttl = 5s;
    unsupported_update.values.leds.rgb = {1U, 2U, 3U};
    REQUIRE(fixture.owner.submit(*opened, unsupported_update).has_value());
    const auto equal_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(equal_snapshot.has_value());
    REQUIRE(scope.engine().publish(*backend, *equal_snapshot).has_value());
    {
        const auto equal_observation = scope.engine().observe(*backend);
        REQUIRE(equal_observation.has_value());
        CHECK(equal_observation->projection_current);
        CHECK(equal_observation->observed_logical_version == equal_snapshot->logical_version);
        CHECK(equal_observation->progress.observed_publication == prior_publication);
    }

    REQUIRE(scope.engine().publish(*backend, *initial).has_value());
    {
        const auto after_old_projection = scope.engine().observe(*backend);
        REQUIRE(after_old_projection.has_value());
        CHECK(after_old_projection->projection_current);
        CHECK(after_old_projection->observed_logical_version == equal_snapshot->logical_version);
        CHECK(after_old_projection->progress.observed_publication == prior_publication);
    }

    unsupported_update.sequence = 3U;
    unsupported_update.values.leds.rgb = {3U, 2U, 1U};
    REQUIRE(fixture.owner.submit(*opened, unsupported_update).has_value());
    {
        const auto newer_unpublished = scope.engine().observe(*backend);
        REQUIRE(newer_unpublished.has_value());
        CHECK(newer_unpublished->projection_current);
        CHECK(newer_unpublished->progress.delivery == delivery_state::setter_accepted);
        CHECK(newer_unpublished->progress.observed_publication == prior_publication);
    }
}

TEST_CASE("output.executor_observation_rejects_old_projection_after_winner_change",
    "[component][output][execution][observation]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> sink =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    {
        const auto empty = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(empty.has_value());
        REQUIRE(scope.engine().publish(*backend, *empty).has_value());
    }
    const auto rumble = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.7F}));
    REQUIRE(rumble.has_value());
    const auto equal_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(equal_snapshot.has_value());
    REQUIRE(scope.engine().publish(*backend, *equal_snapshot).has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    CHECK(scope.engine().drive_one(*lane));
    publication_id prior_publication{};
    {
        const auto completed = scope.engine().observe(*backend);
        REQUIRE(completed.has_value());
        CHECK(completed->projection_current);
        CHECK(completed->progress.delivery == delivery_state::setter_accepted);
        prior_publication = completed->progress.observed_publication;
    }

    const auto winner_update = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.9F, .high = 0.1F});
    REQUIRE(fixture.owner.submit(*opened, winner_update).has_value());
    {
        const auto changed_winner_unpublished = scope.engine().observe(*backend);
        REQUIRE(changed_winner_unpublished.has_value());
        CHECK_FALSE(changed_winner_unpublished->projection_current);
    }

    REQUIRE(scope.engine().publish(*backend, *equal_snapshot).has_value());
    {
        const auto after_old_publish = scope.engine().observe(*backend);
        REQUIRE(after_old_publish.has_value());
        CHECK_FALSE(after_old_publish->projection_current);
        CHECK(after_old_publish->observed_logical_version == equal_snapshot->logical_version);
    }
    const auto latest = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(latest.has_value());
    REQUIRE(scope.engine().publish(*backend, *latest).has_value());
    const auto latest_observation = scope.engine().observe(*backend);
    REQUIRE(latest_observation.has_value());
    CHECK(latest_observation->projection_current);
    CHECK(latest_observation->progress.observed_publication != prior_publication);
    CHECK(latest_observation->progress.delivery == delivery_state::pending);
}

TEST_CASE("output.authority_inspect_audio_qualifies_only_the_exact_current_route",
    "[component][output][observation][audio]") {
    authority_fixture fixture{};
    const auto attached = fixture.trusted.audio_routes().attach(fixture.connection.target,
        body_route_support::hd,
        true);
    REQUIRE(attached.has_value());
    const auto version_before = authority_test_access::logical_version(fixture.owner);
    const auto inspected = fixture.owner.inspect_audio(*attached);
    REQUIRE(inspected.has_value());
    CHECK(inspected->instance == *attached);
    CHECK(inspected->support == body_route_support::hd);
    CHECK(inspected->ready);
    CHECK(authority_test_access::logical_version(fixture.owner) == version_before);

    REQUIRE(fixture.trusted.audio_routes().set_ready(*attached, false).has_value());
    const auto not_ready = fixture.owner.inspect_audio(*attached);
    REQUIRE(not_ready.has_value());
    CHECK_FALSE(not_ready->ready);

    REQUIRE(fixture.trusted.audio_routes().detach(*attached).has_value());
    const auto stale = fixture.owner.inspect_audio(*attached);
    CHECK((!stale.has_value() && stale.error() == error::stale_audio));
    const auto replacement = fixture.trusted.audio_routes().attach(fixture.connection.target,
        body_route_support::classic_only,
        false);
    REQUIRE(replacement.has_value());
    CHECK(*replacement != *attached);
    const auto replacement_observation = fixture.owner.inspect_audio(*replacement);
    REQUIRE(replacement_observation.has_value());
    CHECK(replacement_observation->instance == *replacement);
    CHECK(replacement_observation->support == body_route_support::classic_only);
}

} // namespace
