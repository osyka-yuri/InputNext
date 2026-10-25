#include "body_values.hpp"
#include "values/required_value.hpp"

#include "output/projection/represented.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using namespace inputnext::test::output::body_values;

TEST_CASE("output.body_projection_overlays_rumble_without_changing_discrete_channels",
    "[unit][output]") {
    const auto target = test_target();
    const auto incarnation = test_incarnation();
    const context_identity owner{.target = target,
        .incarnation = incarnation,
        .id = context_id{7U},
        .kind = owner_class::public_game};
    logical_snapshot snapshot{};
    snapshot.target = target;
    snapshot.incarnation = incarnation;
    snapshot.left_trigger =
        channel_snapshot<trigger_value>{.value = {.mode = trigger_mode::weapon, .strength = 0.7F},
            .provenance = {.identity = owner,
                .source_channel = channel::left_trigger,
                .revision = 1U,
                .accepted_sequence = 1U,
                .accepted_commit_order = 1U,
                .priority = internal_priority::game_semantic}};
    snapshot.body = body_snapshot{
        .intent = {.identity = test_identity(audio_instance{.target = target,
                       .authority = incarnation,
                       .id = audio_instance_id{3U}}),
            .revision = 2U,
            .route = body_actuator_route::hd,
            .fallback = body_fallback_reason::none},
        .accepted_sequence = 1U,
        .expires_at = std::chrono::steady_clock::time_point{100ms},
        .fallback_rumble = {.low_frequency = 0.4F, .high_frequency = 0.6F},
    };

    const auto hd_projection =
        project_representation(snapshot, left_trigger_mask | rumble_mask, body_route_support::hd);
    REQUIRE(hd_projection.has_value());
    REQUIRE(hd_projection->body.has_value());
    CHECK_FALSE(hd_projection->rumble.has_value());
    const auto hd_route = represented_body_route(*hd_projection);
    REQUIRE(hd_route.has_value());
    CHECK(required_value(hd_route) == required_value(hd_projection->body).intent);
    const auto hd_output = native_values(*hd_projection);
    CHECK(hd_output.values.rumble == rumble_value{});
    CHECK(hd_output.body.has_value());
    CHECK(required_value(hd_output.body).route == body_actuator_route::hd);
    CHECK(hd_output.values.left_trigger == required_value(snapshot.left_trigger).value);

    const auto classic =
        project_representation(snapshot, rumble_mask, body_route_support::classic_only);
    REQUIRE(classic.has_value());
    const auto classic_output = native_values(*classic);
    CHECK(classic_output.values.rumble == required_value(snapshot.body).fallback_rumble);
    REQUIRE(classic_output.body.has_value());
    CHECK(required_value(classic_output.body).route == body_actuator_route::classic);
    CHECK(required_value(classic_output.body).fallback ==
          body_fallback_reason::unsupported_transport);

    snapshot.rumble =
        channel_snapshot<rumble_value>{.value = {.low_frequency = 0.9F, .high_frequency = 0.8F},
            .provenance = {.identity = owner,
                .source_channel = channel::rumble,
                .revision = 3U,
                .accepted_sequence = 2U,
                .accepted_commit_order = 3U,
                .priority = internal_priority::diagnostic}};
    const auto diagnostic = project_representation(snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(diagnostic.has_value());
    const auto diagnostic_output = native_values(*diagnostic);
    CHECK(diagnostic_output.values.rumble == required_value(snapshot.rumble).value);
    REQUIRE(diagnostic_output.body.has_value());
    CHECK(required_value(diagnostic_output.body).route == body_actuator_route::classic);
    CHECK(required_value(diagnostic_output.body).fallback == body_fallback_reason::preempted);
    const auto diagnostic_route = represented_body_route(*diagnostic);
    REQUIRE(diagnostic_route.has_value());
    CHECK(required_value(diagnostic_route).route == body_actuator_route::classic);
    CHECK(required_value(diagnostic_route).fallback == body_fallback_reason::preempted);
}

TEST_CASE("output.body_projection_omits_hidden_ordinary_rumble_but_restores_it_without_body",
    "[unit][output]") {
    const auto target = test_target();
    const auto incarnation = test_incarnation();
    const context_identity owner{.target = target,
        .incarnation = incarnation,
        .id = context_id{7U},
        .kind = owner_class::public_game};
    logical_snapshot snapshot{};
    snapshot.target = target;
    snapshot.incarnation = incarnation;
    snapshot.body = body_snapshot{
        .intent = {.identity = test_identity(audio_instance{.target = target,
                       .authority = incarnation,
                       .id = audio_instance_id{3U}}),
            .revision = 2U,
            .route = body_actuator_route::hd,
            .fallback = body_fallback_reason::none},
        .accepted_sequence = 1U,
        .expires_at = std::chrono::steady_clock::time_point{100ms},
        .fallback_rumble = {.low_frequency = 0.4F, .high_frequency = 0.6F},
    };
    snapshot.rumble =
        channel_snapshot<rumble_value>{.value = {.low_frequency = 0.2F, .high_frequency = 0.3F},
            .provenance = {.identity = owner,
                .source_channel = channel::rumble,
                .revision = 3U,
                .accepted_sequence = 4U,
                .accepted_commit_order = 5U,
                .priority = internal_priority::game_semantic}};

    const auto initial = project_representation(snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(initial.has_value());
    const auto initial_route = represented_body_route(*initial);
    REQUIRE(initial_route.has_value());

    snapshot.rumble->value = {.low_frequency = 0.8F, .high_frequency = 0.9F};
    snapshot.rumble->provenance.revision = 6U;
    snapshot.rumble->provenance.accepted_commit_order = 8U;
    const auto changed_hidden_value =
        project_representation(snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(changed_hidden_value.has_value());
    CHECK(*changed_hidden_value == *initial);
    CHECK(represented_body_route(*changed_hidden_value) == initial_route);

    const auto latest_rumble = snapshot.rumble;
    snapshot.rumble.reset();
    const auto absent_hidden_value =
        project_representation(snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(absent_hidden_value.has_value());
    CHECK(*absent_hidden_value == *initial);

    snapshot.body.reset();
    snapshot.rumble = latest_rumble;
    const auto restored = project_representation(snapshot, rumble_mask, body_route_support::hd);
    REQUIRE(restored.has_value());
    REQUIRE(restored->rumble.has_value());
    CHECK((native_values(*restored).values.rumble ==
           rumble_value{.low_frequency = 0.8F, .high_frequency = 0.9F}));
}

} // namespace
