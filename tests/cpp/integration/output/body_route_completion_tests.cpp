#include "body_route_test_support.hpp"
#include "cpp/component/output/execution_engine_scope.hpp"
#include "output/authority_fixture.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/release_blocked_write.hpp"
#include "values/required_value.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::body_route_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;
using inputnext::test::output::release_blocked_write;

TEST_CASE("output.body_route_current_native_failures_latch_classic_fallback",
    "[integration][output][body_route]") {
    constexpr std::array failures{native_status::retryable_failure,
        native_status::failed,
        native_status::unsupported,
        native_status::disconnected};
    for (const auto status : failures) {
        authority_fixture fixture;
        const auto audio = fixture.trusted.audio_routes().attach(fixture.connection.target,
            body_route_support::hd,
            true);
        REQUIRE(audio.has_value());
        const auto context = fixture.owner.open(fixture.connection.target);
        REQUIRE(context.has_value());
        const auto body = fixture.owner.submit_body(*context,
            body_request{.audio = *audio,
                .sequence = 1U,
                .ttl = 5s,
                .preference = body_route_preference::prefer_hd,
                .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}});
        REQUIRE(body.has_value());
        execution_engine_scope engine{fixture.owner};
        auto const native_state = std::make_shared<controlled_native_state>();
        native_state->set_next_result(native_result{.status = status});
        std::unique_ptr<native_sink> candidate =
            std::make_unique<controlled_native_sink>(native_state);
        const auto backend = engine.register_backend(fixture.connection.target,
            candidate,
            rumble_mask,
            std::nullopt,
            body_route_support::hd);
        REQUIRE(backend.has_value());
        const auto initial = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(initial.has_value());
        REQUIRE(engine.engine().publish(*backend, *initial).has_value());
        const auto lane = engine.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        REQUIRE(engine.engine().drive_one(*lane));
        REQUIRE(native_state->call_count() == 1U);
        const auto hd_command = native_state->call(0U);
        REQUIRE(hd_command.output.body.has_value());
        CHECK(required_value(hd_command.output.body).route == body_actuator_route::hd);
        const auto fallback = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(fallback.has_value());
        REQUIRE(fallback->body.has_value());
        CHECK(required_value(fallback->body).intent.route == body_actuator_route::classic);
        CHECK(
            required_value(fallback->body).intent.fallback == body_fallback_reason::output_failed);
        const auto progress = engine.engine().progress(*backend);
        REQUIRE(progress.has_value());
        CHECK_FALSE(progress->acknowledged_body_route.has_value());
        if (status == native_status::disconnected) {
            CHECK(progress->phase == backend_phase::retiring);
            continue;
        }
        REQUIRE(engine.engine().publish(*backend, *fallback).has_value());
        REQUIRE(engine.engine().drive_one(*lane));
        REQUIRE(native_state->call_count() == 2U);
        const auto command = native_state->call(1U);
        REQUIRE(command.output.body.has_value());
        CHECK(required_value(command.output.body).route == body_actuator_route::classic);
        CHECK(required_value(command.output.body).fallback == body_fallback_reason::output_failed);
        CHECK(command.output.values.rumble.low_frequency == 0.35F);
        CHECK(command.output.values.rumble.high_frequency == 0.6F);
    }
}

TEST_CASE("output.body_route_stale_disconnection_retires_without_mutating_renewed_lease",
    "[integration][output][body_route][concurrency]") {
    authority_fixture fixture;
    const auto audio = fixture.trusted.audio_routes().attach(fixture.connection.target,
        body_route_support::hd,
        true);
    REQUIRE(audio.has_value());
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const body_request request{.audio = *audio,
        .sequence = 1U,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd,
        .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}};
    const auto initial_body = fixture.owner.submit_body(*context, request);
    REQUIRE(initial_body.has_value());

    execution_engine_scope engine{fixture.owner};
    auto const native_state = std::make_shared<controlled_native_state>();
    native_state->set_next_result(native_result{.status = native_status::disconnected});
    native_state->block_next_write();
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(native_state);
    const auto backend = engine.register_backend(fixture.connection.target,
        candidate,
        rumble_mask,
        std::nullopt,
        body_route_support::hd);
    REQUIRE(backend.has_value());
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(engine.engine().publish(*backend, *snapshot).has_value());
    const auto lane = engine.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    bool drive_result = false;
    std::jthread writer{[&] { drive_result = engine.engine().drive_one(*lane); }};
    const release_blocked_write release{native_state};
    REQUIRE(native_state->wait_for_blocked_write());

    const auto disconnected_command = native_state->call(0U);
    REQUIRE(disconnected_command.output.body.has_value());
    CHECK(required_value(disconnected_command.output.body).route == body_actuator_route::hd);
    const body_request classic{.lease = initial_body->identity.lease,
        .audio = *audio,
        .sequence = 2U,
        .ttl = 5s,
        .preference = body_route_preference::classic,
        .fallback = request.fallback};
    REQUIRE(fixture.owner.submit_body(*context, classic).has_value());
    const body_request renewal{.lease = initial_body->identity.lease,
        .audio = *audio,
        .sequence = 3U,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd,
        .fallback = request.fallback};
    const auto renewed = fixture.owner.submit_body(*context, renewal);
    REQUIRE(renewed.has_value());
    CHECK(renewed->intent.route == body_actuator_route::hd);

    native_state->release_write();
    writer.join();
    CHECK(drive_result);
    const auto progress = engine.engine().progress(*backend);
    REQUIRE(progress.has_value());
    CHECK(progress->phase == backend_phase::retiring);
    CHECK_FALSE(progress->acknowledged_body_route.has_value());

    const auto current = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(current.has_value());
    REQUIRE(current->body.has_value());
    CHECK(required_value(current->body).intent == renewed->intent);
    CHECK(required_value(current->body).intent.route == body_actuator_route::hd);
    CHECK(native_state->call_count() == 1U);
}

TEST_CASE("output.body_route_target_retirement_rejects_late_accepted_completion",
    "[integration][output][body_route][concurrency]") {
    authority_fixture fixture;
    const auto audio = fixture.trusted.audio_routes().attach(fixture.connection.target,
        body_route_support::hd,
        true);
    REQUIRE(audio.has_value());
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const body_request request{.audio = *audio,
        .sequence = 1U,
        .ttl = 5s,
        .preference = body_route_preference::prefer_hd,
        .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}};
    REQUIRE(fixture.owner.submit_body(*context, request).has_value());

    execution_engine_scope engine{fixture.owner};
    const auto native_state = std::make_shared<controlled_native_state>();
    native_state->block_next_write();
    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(native_state);
    const auto backend = engine.register_backend(fixture.connection.target,
        candidate,
        rumble_mask,
        std::nullopt,
        body_route_support::hd);
    REQUIRE(backend.has_value());
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(engine.engine().publish(*backend, *snapshot).has_value());
    const auto lane = engine.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    bool drive_result = false;
    std::jthread writer{[&] { drive_result = engine.engine().drive_one(*lane); }};
    const release_blocked_write release{native_state};
    REQUIRE(native_state->wait_for_blocked_write());
    REQUIRE(fixture.registry.retire(fixture.connection.target).has_value());

    native_state->release_write();
    writer.join();
    CHECK(drive_result);
    const auto progress = engine.engine().progress(*backend);
    REQUIRE(progress.has_value());
    CHECK(progress->phase == backend_phase::retiring);
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    REQUIRE(progress->last_completed_attempt.has_value());
    CHECK(
        required_value(progress->last_completed_attempt).result.status == native_status::accepted);
    const auto receipt = engine.engine().retire(*backend);
    REQUIRE(receipt.has_value());
    CHECK(receipt->reason == retirement_reason::connection_retired);
}

TEST_CASE("output.body_route_default_backend_projects_hd_intent_as_unsupported_fallback",
    "[integration][output][body_route]") {
    body_route_fixture fixture;
    REQUIRE(fixture.attach_audio(body_route_support::hd).has_value());
    REQUIRE(fixture.open_context().has_value());
    const auto accepted = fixture.acquire_body();
    REQUIRE(accepted.has_value());
    CHECK(accepted->intent.route == body_actuator_route::hd);
    fixture.running->request_drive();

    REQUIRE(fixture.native_state->wait_for_write_count(2U));
    const auto command = fixture.native_state->call(1U);
    REQUIRE(command.output.body.has_value());
    CHECK(required_value(command.output.body).route == body_actuator_route::classic);
    CHECK(required_value(command.output.body).fallback ==
          body_fallback_reason::unsupported_transport);
    CHECK(command.output.values.rumble.low_frequency == 0.35F);
    CHECK(command.output.values.rumble.high_frequency == 0.6F);

    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent == accepted->intent);
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    const body_route_ack not_hd_ack{.backend = fixture.backend,
        .publication = command.publication,
        .attempt = command.attempt,
        .intent = required_value(command.output.body)};
    const auto qualification = fixture.running->qualifies_route(not_hd_ack);
    REQUIRE(qualification.has_value());
    CHECK_FALSE(*qualification);
}

} // namespace
