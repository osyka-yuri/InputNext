#include "output/authority_fixture.hpp"

#include "execution_engine_scope.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

TEST_CASE("output.execution_attempt_uses_real_native_sink_and_exact_completion",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    CHECK(scope.engine().drive_one(*lane));

    const auto calls = sink_state->call_count();
    const auto command = sink_state->call(0U);
    auto progress = scope.engine().progress(*backend);
    REQUIRE(progress.has_value());
    CHECK(calls == 1U);
    CHECK(command.backend == *backend);
    CHECK(command.publication == progress->observed_publication);
    CHECK(command.attempt == attempt_id{1U});
    CHECK(command.output.channels == rumble_mask);
    CHECK(command.output.values.rumble == rumble_value{});
    CHECK(progress->delivery == delivery_state::setter_accepted);
    const auto latest_attempt = progress->last_attempt.value_or(attempt_id{});
    CHECK(latest_attempt == command.attempt);
    const auto completed = progress->last_completed_attempt.value_or(completed_attempt{});
    CHECK(completed.id == command.attempt);
    CHECK(completed.publication == command.publication);
    CHECK(completed.result.status == native_status::accepted);
    CHECK(completed.output == command.output);
    CHECK(!progress->write_in_flight);
}

TEST_CASE("output.execution_late_success_does_not_confirm_a_new_publication",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    std::jthread drive_thread([&scope, lane_token_value = *lane] {
        static_cast<void>(scope.engine().drive_one(lane_token_value));
    });
    const bool entered = sink_state->wait_for_blocked_write();
    bool mutation_accepted = false;
    bool publication_accepted = false;
    if (entered) {
        auto context = fixture.owner.open(fixture.connection.target);
        if (context.has_value()) {
            const auto request = inputnext::test::output::rumble_request({.channels = rumble_mask,
                .sequence = 1U,
                .ttl = std::chrono::seconds{5},
                .low = 0.4F,
                .high = 0.7F});
            const auto accepted = fixture.owner.submit(*context, request);
            mutation_accepted = accepted.has_value();
            auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
            if (snapshot.has_value()) {
                publication_accepted = scope.engine().publish(*backend, *snapshot).has_value();
            }
        }
    }
    sink_state->release_write();
    drive_thread.join();
    CHECK(entered);
    CHECK(mutation_accepted);
    CHECK(publication_accepted);

    auto pending = scope.engine().progress(*backend);
    REQUIRE(pending.has_value());
    CHECK(pending->delivery == delivery_state::pending);
    CHECK(pending->observed_publication.value == 2U);
    const auto previous_completion = pending->last_completed_attempt.value_or(completed_attempt{});
    CHECK(previous_completion.publication.value == 1U);
    const auto previous_command = sink_state->call(0U);
    CHECK(previous_completion.output == previous_command.output);
    CHECK(scope.engine().drive_one(*lane));
    const auto current_command = sink_state->call(1U);
    CHECK(current_command.publication == pending->observed_publication);
    CHECK(current_command.output.values.rumble.low_frequency == 0.4F);
    CHECK(current_command.output.values.rumble.high_frequency == 0.7F);
    CHECK(previous_completion.output != current_command.output);
    auto completed = scope.engine().progress(*backend);
    REQUIRE(completed.has_value());
    CHECK(completed->delivery == delivery_state::setter_accepted);
    const auto latest_completion = completed->last_completed_attempt.value_or(completed_attempt{});
    CHECK(latest_completion.id == current_command.attempt);
    CHECK(latest_completion.output == current_command.output);
}

} // namespace
