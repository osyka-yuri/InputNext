#include "body_route_manual_fixture.hpp"
#include "output/release_blocked_write.hpp"
#include "values/required_value.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::required_value;
using inputnext::test::output::manual_body_route_fixture;
using inputnext::test::output::release_blocked_write;

[[nodiscard]] submit_request trigger_request(const std::uint64_t sequence,
    const trigger_value value) noexcept {
    submit_request request;
    request.selected_channels = left_trigger_mask;
    request.sequence = sequence;
    request.ttl = 5s;
    request.values.left_trigger = value;
    return request;
}

[[nodiscard]] submit_request led_request(const std::uint64_t sequence,
    const led_value value) noexcept {
    submit_request request;
    request.selected_channels = leds_mask;
    request.sequence = sequence;
    request.ttl = 5s;
    request.values.leds = value;
    return request;
}

TEST_CASE("output.body_route_ack_survives_trigger_led_publications_and_blocked_same_route_write",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto acknowledgement = fixture.accepted_ack();

    const auto trigger = trigger_request(1U, {.mode = trigger_mode::weapon, .strength = 0.6F});
    REQUIRE(fixture.submit(fixture.context, trigger).has_value());
    const auto with_trigger = fixture.snapshot();
    REQUIRE(with_trigger.has_value());
    auto qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);
    REQUIRE(fixture.publish(*with_trigger).has_value());
    auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(progress->delivery == delivery_state::pending);
    qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);

    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    const auto leds = led_request(2U, {.rgb = {0x20U, 0x40U, 0x60U}, .player_leds = 3U});
    REQUIRE(fixture.submit(fixture.context, leds).has_value());
    const auto latest = fixture.snapshot();
    REQUIRE(latest.has_value());
    REQUIRE(fixture.publish(*latest).has_value());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->acknowledged_body_route == acknowledgement);
    qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);

    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->last_completed_attempt.has_value());
    CHECK(
        required_value(progress->last_completed_attempt).result.status == native_status::accepted);
    CHECK(required_value(progress->last_completed_attempt).id != acknowledgement.attempt);
    CHECK(required_value(progress->last_completed_attempt).publication !=
          progress->observed_publication);
    CHECK(progress->acknowledged_body_route == acknowledgement);
    CHECK(progress->delivery == delivery_state::pending);
    REQUIRE(fixture.drive());
    CHECK(fixture.native_state->call_count() == 3U);
    const auto delivered = fixture.native_state->call(2U);
    CHECK(delivered.output.values.left_trigger == trigger.values.left_trigger);
    CHECK(delivered.output.values.leds == leds.values.leds);
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->delivery == delivery_state::setter_accepted);
    CHECK(required_value(progress->last_completed_attempt).id != acknowledgement.attempt);
    CHECK(required_value(progress->last_completed_attempt).publication ==
          progress->observed_publication);
    CHECK(progress->acknowledged_body_route == acknowledgement);
    qualification = fixture.qualifies(acknowledgement);
    REQUIRE(qualification.has_value());
    CHECK(*qualification);
}

TEST_CASE("output.body_route_initial_blocked_write_acks_route_after_unrelated_changes",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());
    const auto command = fixture.native_state->call(0U);
    REQUIRE(command.output.body.has_value());
    CHECK(required_value(command.output.body).route == body_actuator_route::hd);
    auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());

    const auto trigger = trigger_request(1U, {.mode = trigger_mode::feedback, .strength = 0.4F});
    REQUIRE(fixture.submit(fixture.context, trigger).has_value());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->observed_publication == command.publication);

    const auto leds = led_request(2U, {.rgb = {0x80U, 0x20U, 0x10U}, .microphone_led = true});
    REQUIRE(fixture.submit(fixture.context, leds).has_value());
    const auto latest = fixture.snapshot();
    REQUIRE(latest.has_value());
    REQUIRE(fixture.publish(*latest).has_value());

    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    REQUIRE(progress->acknowledged_body_route.has_value());
    const auto acknowledgement = required_value(progress->acknowledged_body_route);
    CHECK(acknowledgement.publication == command.publication);
    CHECK(acknowledgement.attempt == command.attempt);
    CHECK(progress->observed_publication != command.publication);
    CHECK(progress->delivery == delivery_state::pending);
    const auto qualified = fixture.qualifies(acknowledgement);
    REQUIRE(qualified.has_value());
    CHECK(*qualified);

    REQUIRE(fixture.drive());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->delivery == delivery_state::setter_accepted);
    CHECK(progress->acknowledged_body_route == acknowledgement);
}

TEST_CASE("output.body_route_retry_during_blocked_write_survives_unpublished_channel_change",
    "[integration][output][body_route][concurrency]") {
    manual_body_route_fixture fixture;
    const auto initial = fixture.snapshot();
    REQUIRE(initial.has_value());
    REQUIRE(fixture.publish(*initial).has_value());
    REQUIRE(fixture.drive());
    const auto initial_ack = fixture.accepted_ack();

    const auto trigger = trigger_request(1U, {.mode = trigger_mode::weapon, .strength = 0.5F});
    REQUIRE(fixture.submit(fixture.context, trigger).has_value());
    const auto with_trigger = fixture.snapshot();
    REQUIRE(with_trigger.has_value());
    REQUIRE(fixture.publish(*with_trigger).has_value());
    fixture.native_state->block_next_write();
    bool drive_result = false;
    std::jthread writer{[&] { drive_result = fixture.drive(); }};
    release_blocked_write unblock{fixture.native_state};
    REQUIRE(fixture.native_state->wait_for_blocked_write());

    REQUIRE(fixture.engine->engine().request_retry(fixture.backend).has_value());
    auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->retry_requested);
    CHECK_FALSE(progress->acknowledged_body_route.has_value());

    const auto leds = led_request(2U, {.rgb = {0x10U, 0x30U, 0x90U}, .player_leds = 1U});
    REQUIRE(fixture.submit(fixture.context, leds).has_value());
    fixture.native_state->release_write();
    writer.join();
    CHECK(drive_result);
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->retry_requested);
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    REQUIRE(progress->last_completed_attempt.has_value());
    CHECK(
        required_value(progress->last_completed_attempt).result.status == native_status::accepted);
    CHECK(required_value(progress->last_completed_attempt).id.value ==
          initial_ack.attempt.value + 1U);

    const auto latest = fixture.snapshot();
    REQUIRE(latest.has_value());
    REQUIRE(fixture.publish(*latest).has_value());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK(progress->retry_requested);
    REQUIRE(fixture.drive());
    progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->retry_requested);
    REQUIRE(progress->acknowledged_body_route.has_value());
    REQUIRE(progress->last_completed_attempt.has_value());
    CHECK(required_value(progress->acknowledged_body_route).attempt ==
          required_value(progress->last_completed_attempt).id);
    CHECK(required_value(progress->acknowledged_body_route).attempt != initial_ack.attempt);
}

} // namespace
