#include "body_route_manual_fixture.hpp"
#include "output/release_blocked_write.hpp"
#include "values/required_value.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::required_value;
using inputnext::test::output::manual_body_route_fixture;
using inputnext::test::output::release_blocked_write;

[[nodiscard]] submit_request trigger_request() noexcept {
    submit_request request;
    request.selected_channels = left_trigger_mask;
    request.sequence = 1U;
    request.ttl = 5s;
    request.values.left_trigger = {.mode = trigger_mode::weapon, .strength = 0.7F};
    return request;
}

[[nodiscard]] submit_request led_request() noexcept {
    submit_request request;
    request.selected_channels = leds_mask;
    request.sequence = 2U;
    request.ttl = 5s;
    request.values.leds = {.rgb = {0x30U, 0x60U, 0x90U}, .player_leds = 2U};
    return request;
}

constexpr std::array native_failures{native_status::retryable_failure,
    native_status::unsupported,
    native_status::failed,
    native_status::disconnected};

void verify_failed_same_route_write(const native_status status, const bool request_retry) {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto acknowledgement = fixture.accepted_ack();

    REQUIRE(fixture.submit(fixture.context, trigger_request()).has_value());
    const auto with_trigger = fixture.snapshot();
    REQUIRE(with_trigger.has_value());
    REQUIRE(fixture.publish(*with_trigger).has_value());
    fixture.native_state->set_next_result(native_result{.status = status});
    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());
    if (request_retry) {
        REQUIRE(fixture.engine->engine().request_retry(fixture.backend).has_value());
    }

    REQUIRE(fixture.submit(fixture.context, led_request()).has_value());
    const auto with_led = fixture.snapshot();
    REQUIRE(with_led.has_value());
    REQUIRE(fixture.publish(*with_led).has_value());
    const auto pending = fixture.progress();
    REQUIRE(pending.has_value());
    if (request_retry) {
        CHECK(pending->retry_requested);
        CHECK(!pending->acknowledged_body_route.has_value());
    } else {
        CHECK(pending->acknowledged_body_route == acknowledgement);
        const auto qualified = fixture.qualifies(acknowledgement);
        REQUIRE(qualified.has_value());
        CHECK(*qualified);
    }

    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);

    const auto current = fixture.snapshot();
    REQUIRE(current.has_value());
    REQUIRE(current->body.has_value());
    CHECK(required_value(current->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(current->body).intent.fallback == body_fallback_reason::output_failed);

    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    REQUIRE(progress->last_completed_attempt.has_value());
    CHECK(required_value(progress->last_completed_attempt).result.status == status);
    if (status == native_status::disconnected) {
        CHECK(progress->phase == backend_phase::retiring);
    } else {
        CHECK(progress->phase == backend_phase::current);
        CHECK(progress->retry_requested == request_retry);
        CHECK(progress->delivery == delivery_state::pending);
        CHECK(fixture.native_state->call_count() == 2U);
    }
}

TEST_CASE("output.body_route_nonaccepted_write_falls_back_despite_unrelated_publication",
    "[integration][output][body_route][concurrency]") {
    for (const auto status : native_failures) {
        verify_failed_same_route_write(status, false);
    }
}

TEST_CASE("output.body_route_nonaccepted_write_preserves_pending_retry_after_fallback",
    "[integration][output][body_route][concurrency]") {
    for (const auto status : native_failures) {
        verify_failed_same_route_write(status, true);
    }
}

TEST_CASE("output.body_route_stale_failure_does_not_fallback_a_renewed_route",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());

    REQUIRE(fixture.submit(fixture.context, trigger_request()).has_value());
    const auto with_trigger = fixture.snapshot();
    REQUIRE(with_trigger.has_value());
    REQUIRE(fixture.publish(*with_trigger).has_value());
    fixture.native_state->set_next_result(native_result{.status = native_status::failed});
    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    const body_request classic{.lease = fixture.body.identity.lease,
        .audio = fixture.audio,
        .sequence = 2U,
        .ttl = 5s,
        .preference = body_route_preference::classic,
        .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}};
    REQUIRE(fixture.authority.owner.submit_body(fixture.context, classic).has_value());
    const auto renewed = fixture.authority.owner.submit_body(fixture.context,
        body_request{.lease = fixture.body.identity.lease,
            .audio = fixture.audio,
            .sequence = 3U,
            .ttl = 5s,
            .preference = body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}});
    REQUIRE(renewed.has_value());
    CHECK(renewed->intent.route == body_actuator_route::hd);

    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);

    const auto current = fixture.snapshot();
    REQUIRE(current.has_value());
    REQUIRE(current->body.has_value());
    CHECK(required_value(current->body).intent == renewed->intent);
    CHECK(required_value(current->body).intent.route == body_actuator_route::hd);
    CHECK(required_value(current->body).intent.fallback == body_fallback_reason::none);
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    REQUIRE(progress->last_completed_attempt.has_value());
    CHECK(required_value(progress->last_completed_attempt).result.status == native_status::failed);
}

TEST_CASE("output.body_route_failure_after_keep_still_invalidates_unchanged_route",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());

    REQUIRE(fixture.submit(fixture.context, trigger_request()).has_value());
    const auto with_trigger = fixture.snapshot();
    REQUIRE(with_trigger.has_value());
    REQUIRE(fixture.publish(*with_trigger).has_value());
    fixture.native_state->set_next_result(native_result{.status = native_status::failed});
    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    const release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    const auto renewed = fixture.authority.owner.submit_body(fixture.context,
        body_request{.lease = fixture.body.identity.lease,
            .audio = fixture.audio,
            .sequence = 2U,
            .ttl = 5s,
            .preference = body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.35F, .high_frequency = 0.6F}});
    REQUIRE(renewed.has_value());
    CHECK(renewed->intent == fixture.body.intent);
    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);

    const auto current = fixture.snapshot();
    REQUIRE(current.has_value());
    REQUIRE(current->body.has_value());
    CHECK(required_value(current->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(current->body).intent.fallback == body_fallback_reason::output_failed);
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
}

} // namespace
