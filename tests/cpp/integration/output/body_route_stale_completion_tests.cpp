#include "body_route_test_support.hpp"
#include "output/authority_fixture.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/release_blocked_write.hpp"
#include "values/required_value.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace {

using inputnext::test::required_value;

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::body_route_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::make_internal_producer;
using inputnext::test::output::release_blocked_write;

[[nodiscard]] native_command block_hd_attempt(body_route_fixture& fixture,
    const std::chrono::milliseconds ttl = 5s) {
    fixture.native_state->block_next_write();
    REQUIRE(fixture.acquire_body(ttl).has_value());
    fixture.running->request_drive();
    REQUIRE(fixture.native_state->wait_for_blocked_write());
    const auto count = fixture.native_state->call_count();
    REQUIRE(count >= 2U);
    return fixture.native_state->call(count - 1U);
}

[[nodiscard]] bool route_qualifies(body_route_fixture& fixture, const native_command& command) {
    const auto result = fixture.running->qualifies_route(body_route_ack{.backend = command.backend,
        .publication = command.publication,
        .attempt = command.attempt,
        .intent = required_value(command.output.body)});
    return result.value_or(false);
}

[[nodiscard]] bool wait_for_backend_completion(body_route_fixture& fixture,
    const backend_handle backend,
    const attempt_id attempt) {
    return fixture.wait_until([&fixture, backend, attempt] {
        const auto current = fixture.running->progress(backend);
        return current.has_value() && current->last_completed_attempt.has_value() &&
               required_value(current->last_completed_attempt).id == attempt;
    });
}

TEST_CASE("output.body_route_late_accepted_completion_after_renewal_cannot_ack_old_revision",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto old_command = block_hd_attempt(fixture);
    REQUIRE(old_command.output.body.has_value());
    CHECK(required_value(old_command.output.body).route == body_actuator_route::hd);

    const auto renewed = fixture.renew_body(2U);
    REQUIRE(renewed.has_value());
    CHECK(renewed->intent.revision != required_value(old_command.output.body).revision);
    fixture.native_state->release_write();

    CHECK_FALSE(route_qualifies(fixture, old_command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));
    const auto current = fixture.progress();
    REQUIRE(current.has_value());
    REQUIRE(current->acknowledged_body_route.has_value());
    CHECK(required_value(current->acknowledged_body_route).intent == renewed->intent);
    CHECK(required_value(current->acknowledged_body_route).attempt != old_command.attempt);
    CHECK(fixture.native_state->call(2U).output.body == renewed->intent);
}

TEST_CASE("output.body_route_late_accepted_completion_after_clear_cannot_ack",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture);

    REQUIRE(fixture.clear_body(2U).has_value());
    fixture.native_state->release_write();
    CHECK_FALSE(route_qualifies(fixture, command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));

    const auto fallback = fixture.native_state->call(2U);
    CHECK_FALSE(fallback.output.body.has_value());
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->body.has_value());
}

TEST_CASE("output.body_route_expiry_during_native_write_rejects_late_completion",
    "[integration][output][body_route][concurrency]") {
    std::atomic<std::int64_t> ticks{};
    body_route_fixture fixture{body_route_support::hd, &ticks};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture, 5ms);

    ticks.store(std::chrono::duration_cast<std::chrono::steady_clock::duration>(5ms).count(),
        std::memory_order_release);
    const auto expired = fixture.snapshot();
    REQUIRE(expired.has_value());
    CHECK_FALSE(expired->body.has_value());
    fixture.native_state->release_write();
    CHECK_FALSE(route_qualifies(fixture, command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
}

TEST_CASE("output.body_route_context_close_rejects_late_hd_completion",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture);

    REQUIRE(fixture.close_context().has_value());
    fixture.native_state->release_write();
    CHECK_FALSE(route_qualifies(fixture, command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->body.has_value());
}

TEST_CASE("output.body_route_audio_detach_and_replacement_do_not_transfer_old_lease",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    const auto old_audio = required_value(fixture.audio);
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture);

    REQUIRE(fixture.detach_audio().has_value());
    const auto replacement = fixture.attach_audio(body_route_support::hd);
    REQUIRE(replacement.has_value());
    CHECK(*replacement != old_audio);
    fixture.native_state->release_write();
    CHECK_FALSE(route_qualifies(fixture, command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));

    const auto snapshot = fixture.snapshot();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->body.has_value());
    CHECK(required_value(snapshot->body).intent.identity.audio == old_audio);
    CHECK(required_value(snapshot->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(snapshot->body).intent.fallback == body_fallback_reason::instance_lost);
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
}

TEST_CASE("output.body_route_diagnostic_appearance_and_removal_latch_classic",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture);

    const internal_context_options options{.owner = internal_owner_key{0xB017U},
        .priority = internal_priority::diagnostic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background};
    auto producer = make_internal_producer(fixture.authority.trusted, options);
    const auto diagnostic = producer.open(fixture.authority.connection.target);
    REQUIRE(diagnostic.has_value());
    const auto submitted = producer.submit(*diagnostic,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.9F, .high = 0.9F}));
    REQUIRE(submitted.has_value());

    const clear_request clear{.selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = submitted->resulting_revisions};
    REQUIRE(producer.clear(*diagnostic, clear).has_value());

    const auto latched = fixture.snapshot();
    REQUIRE(latched.has_value());
    REQUIRE(latched->body.has_value());
    CHECK(required_value(latched->body).intent.route == body_actuator_route::classic);
    CHECK(required_value(latched->body).intent.fallback == body_fallback_reason::preempted);
    fixture.native_state->release_write();
    CHECK_FALSE(route_qualifies(fixture, command));
    REQUIRE(fixture.native_state->wait_for_write_count(3U));
    const auto progress = fixture.progress();
    REQUIRE(progress.has_value());
    CHECK_FALSE(progress->acknowledged_body_route.has_value());
    const auto next_command = fixture.native_state->call(2U);
    REQUIRE(next_command.output.body.has_value());
    CHECK(required_value(next_command.output.body).route == body_actuator_route::classic);
    CHECK(required_value(next_command.output.body).fallback == body_fallback_reason::preempted);
}

TEST_CASE("output.body_route_backend_replacement_cannot_ack_old_backend_write",
    "[integration][output][body_route][concurrency]") {
    body_route_fixture fixture{body_route_support::hd};
    REQUIRE(fixture.attach_audio().has_value());
    REQUIRE(fixture.open_context().has_value());
    release_blocked_write release{fixture.native_state};
    const auto command = block_hd_attempt(fixture);
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
    fixture.native_state->release_write();
    REQUIRE(wait_for_backend_completion(fixture, old_backend, command.attempt));
    REQUIRE(replacement_state->wait_for_write_count(1U));
    REQUIRE(fixture.wait_until([&fixture] {
        const auto current = fixture.progress();
        return current.has_value() && current->acknowledged_body_route.has_value();
    }));

    const auto old_progress = fixture.running->progress(old_backend);
    REQUIRE(old_progress.has_value());
    CHECK(old_progress->phase != backend_phase::current);
    CHECK_FALSE(old_progress->acknowledged_body_route.has_value());
    const auto current = fixture.progress();
    REQUIRE(current.has_value());
    REQUIRE(current->acknowledged_body_route.has_value());
    CHECK(required_value(current->acknowledged_body_route).backend == *replacement);
    CHECK(required_value(current->acknowledged_body_route).intent ==
          required_value(fixture.body).intent);
}

} // namespace
