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

TEST_CASE("output.body_route_readiness_is_not_ack_and_accepted_route_stays_stable",
    "[integration][output][body_route]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());

    fixture.native_state->block_next_write();
    release_blocked_write release{fixture.native_state};
    REQUIRE(fixture.acquire_body().has_value());
    fixture.running->request_drive();
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    const auto in_flight = fixture.progress();
    REQUIRE(in_flight.has_value());
    CHECK(in_flight->write_in_flight);
    CHECK_FALSE(in_flight->acknowledged_body_route.has_value());
    const auto command = fixture.native_state->call(1U);
    REQUIRE(command.output.body.has_value());
    CHECK(required_value(command.output.body).route == body_actuator_route::hd);

    const body_route_ack pending_ack{.backend = fixture.backend,
        .publication = command.publication,
        .attempt = command.attempt,
        .intent = required_value(command.output.body)};
    const auto pending_qualification = fixture.running->qualifies_route(pending_ack);
    REQUIRE(pending_qualification.has_value());
    CHECK_FALSE(*pending_qualification);

    fixture.native_state->release_write();
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));

    const auto accepted = fixture.progress();
    REQUIRE(accepted.has_value());
    REQUIRE(accepted->acknowledged_body_route.has_value());
    const auto acknowledgement = required_value(accepted->acknowledged_body_route);
    const auto qualified = fixture.running->qualifies_route(acknowledgement);
    REQUIRE(qualified.has_value());
    CHECK(*qualified);

    fixture.running->request_drive();
    std::this_thread::sleep_for(24ms);
    CHECK(fixture.native_state->call_count() == 2U);
    const auto still_qualified = fixture.running->qualifies_route(acknowledgement);
    REQUIRE(still_qualified.has_value());
    CHECK(*still_qualified);
    REQUIRE(fixture.running->retire(fixture.backend).has_value());
    const auto retired = fixture.progress();
    REQUIRE(retired.has_value());
    CHECK_FALSE(retired->acknowledged_body_route.has_value());
}

TEST_CASE("output.body_route_retry_invalidates_the_acknowledged_attempt",
    "[integration][output][body_route]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    REQUIRE(fixture.acquire_body().has_value());
    fixture.running->request_drive();

    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto accepted = fixture.progress();
    REQUIRE(accepted.has_value());
    const auto old_acknowledgement = required_value(accepted->acknowledged_body_route);

    REQUIRE(fixture.running->request_retry(fixture.backend).has_value());
    const auto old_qualification = fixture.running->qualifies_route(old_acknowledgement);
    REQUIRE(old_qualification.has_value());
    CHECK_FALSE(*old_qualification);

    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto retried = fixture.progress();
    REQUIRE(retried.has_value());
    REQUIRE(retried->acknowledged_body_route.has_value());
    CHECK(required_value(retried->acknowledged_body_route).publication ==
          old_acknowledgement.publication);
    CHECK(required_value(retried->acknowledged_body_route).attempt != old_acknowledgement.attempt);
    CHECK(fixture.native_state->call_count() == 3U);
}

TEST_CASE("output.body_route_retry_requested_during_write_does_not_ack_that_attempt",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    fixture.native_state->block_next_write();
    release_blocked_write release{fixture.native_state};
    REQUIRE(fixture.acquire_body().has_value());
    fixture.running->request_drive();
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    const auto command = fixture.native_state->call(1U);
    REQUIRE(command.output.body.has_value());
    REQUIRE(required_value(command.output.body).route == body_actuator_route::hd);
    const body_route_ack in_flight_ack{.backend = fixture.backend,
        .publication = command.publication,
        .attempt = command.attempt,
        .intent = required_value(command.output.body)};
    REQUIRE(fixture.running->request_retry(fixture.backend).has_value());
    const auto retrying = fixture.progress();
    REQUIRE(retrying.has_value());
    CHECK(retrying->retry_requested);
    CHECK_FALSE(retrying->acknowledged_body_route.has_value());

    fixture.native_state->release_write();
    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto retried = fixture.progress();
    REQUIRE(retried.has_value());
    REQUIRE(retried->acknowledged_body_route.has_value());
    CHECK(required_value(retried->acknowledged_body_route).attempt != in_flight_ack.attempt);
    CHECK(fixture.native_state->call(1U).attempt == in_flight_ack.attempt);
    CHECK(fixture.native_state->call(2U).attempt ==
          required_value(retried->acknowledged_body_route).attempt);
}

TEST_CASE("output.body_route_backend_replacement_clears_acknowledgement",
    "[integration][output][body_route]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    REQUIRE(fixture.acquire_body().has_value());
    fixture.running->request_drive();
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto accepted = fixture.progress();
    REQUIRE(accepted.has_value());
    const auto old_acknowledgement = required_value(accepted->acknowledged_body_route);
    const auto old_backend = fixture.backend;

    auto const replacement_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(replacement_state);
    const auto replacement = fixture.running->replace_backend(old_backend,
        fixture.authority.connection.target,
        candidate,
        rumble_mask,
        body_route_support::hd);
    REQUIRE(replacement.has_value());
    fixture.backend = *replacement;

    const auto old_route = fixture.running->progress(old_backend);
    REQUIRE(old_route.has_value());
    CHECK_FALSE(old_route->acknowledged_body_route.has_value());
    const auto old_qualification = fixture.running->qualifies_route(old_acknowledgement);
    REQUIRE(old_qualification.has_value());
    CHECK_FALSE(*old_qualification);
    REQUIRE(replacement_state->wait_for_write_count(1U));
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto current = fixture.progress();
    REQUIRE(current.has_value());
    REQUIRE(current->acknowledged_body_route.has_value());
    CHECK(required_value(current->acknowledged_body_route).backend == *replacement);
}

TEST_CASE("output.body_route_ack_is_requalified_against_authority_before_refresh",
    "[integration][output][body_route]") {
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
        .fallback = {.low_frequency = 0.3F, .high_frequency = 0.7F}};
    const auto accepted_body = fixture.owner.submit_body(*context, request);
    REQUIRE(accepted_body.has_value());

    execution_engine_scope engine{fixture.owner};
    const auto native_state = std::make_shared<controlled_native_state>();
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
    REQUIRE(engine.engine().drive_one(*lane));

    const auto completed = engine.engine().progress(*backend);
    REQUIRE(completed.has_value());
    REQUIRE(completed->acknowledged_body_route.has_value());
    const auto acknowledgement = required_value(completed->acknowledged_body_route);
    const auto current = engine.engine().qualifies_route(acknowledgement);
    REQUIRE(current.has_value());
    CHECK(*current);

    const body_request renewal{.lease = accepted_body->identity.lease,
        .audio = *audio,
        .sequence = 2U,
        .ttl = 5s,
        .preference = body_route_preference::classic,
        .fallback = request.fallback};
    REQUIRE(fixture.owner.submit_body(*context, renewal).has_value());

    const auto stale = engine.engine().qualifies_route(acknowledgement);
    REQUIRE(stale.has_value());
    CHECK_FALSE(*stale);
    const auto retained = engine.engine().progress(*backend);
    REQUIRE(retained.has_value());
    CHECK(retained->acknowledged_body_route == acknowledgement);

    const auto renewed_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(renewed_snapshot.has_value());
    REQUIRE(engine.engine().publish(*backend, *renewed_snapshot).has_value());
    const auto republished = engine.engine().progress(*backend);
    REQUIRE(republished.has_value());
    CHECK_FALSE(republished->acknowledged_body_route.has_value());
}

} // namespace
