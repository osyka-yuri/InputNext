#include "output/authority_fixture.hpp"

#include "execution_engine_scope.hpp"
#include "execution_engine_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

TEST_CASE("output.execution_retryable_failure_redrives_same_publication_after_backoff",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->set_next_result(native_result{.status = native_status::retryable_failure});
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    CHECK(scope.engine().drive_one(*lane));
    auto failed = scope.engine().progress(*backend);
    REQUIRE(failed.has_value());
    CHECK(failed->delivery == delivery_state::retry_wait);
    CHECK(failed->consecutive_retryable_failures == 1U);
    CHECK(failed->scheduled_retry_delay == std::chrono::milliseconds{4});
    REQUIRE(failed->last_completed_attempt.has_value());
    const auto failed_attempt = failed->last_completed_attempt.value_or(completed_attempt{});
    CHECK(failed_attempt.result.status == native_status::retryable_failure);
    CHECK(failed_attempt.output == sink_state->call(0U).output);

    std::stop_source stop_source;
    std::mutex result_mutex;
    std::condition_variable completed;
    bool returned = false;
    bool ready = false;
    std::thread waiter([&] {
        const bool result = scope.engine().wait_for_work(*lane, stop_source.get_token());
        {
            std::scoped_lock lock{result_mutex};
            ready = result;
            returned = true;
        }
        completed.notify_all();
    });
    bool wait_finished = false;
    {
        std::unique_lock lock{result_mutex};
        wait_finished = completed.wait_for(lock, std::chrono::seconds{2}, [&] { return returned; });
    }
    if (!wait_finished) {
        stop_source.request_stop();
    }
    waiter.join();
    CHECK(wait_finished);
    CHECK(ready);
    CHECK(scope.engine().drive_one(*lane));

    CHECK(sink_state->call_count() == 2U);
    CHECK(sink_state->call(0U).publication == sink_state->call(1U).publication);
    CHECK(sink_state->call(0U).output == sink_state->call(1U).output);
    CHECK(sink_state->call(0U).attempt == attempt_id{1U});
    CHECK(sink_state->call(1U).attempt == attempt_id{2U});
    auto accepted = scope.engine().progress(*backend);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
    REQUIRE(accepted->last_completed_attempt.has_value());
    const auto accepted_attempt = accepted->last_completed_attempt.value_or(completed_attempt{});
    CHECK(accepted_attempt.id == attempt_id{2U});
}

TEST_CASE("output.retry_during_inflight_write_survives_accepted_same_value_completion",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 3U));
    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    std::jthread drive_thread(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const bool entered = sink_state->wait_for_blocked_write();
    const auto before_retry = scope.engine().progress(*backend);
    bool retry_accepted = false;
    if (entered) {
        for (int request = 0; request < 3; ++request) {
            retry_accepted = scope.engine().request_retry(*backend).has_value() || retry_accepted;
        }
    }
    const auto after_retry = scope.engine().progress(*backend);
    sink_state->release_write();
    drive_thread.join();

    CHECK(entered);
    CHECK(retry_accepted);
    REQUIRE(before_retry.has_value());
    CHECK(before_retry->write_in_flight);
    CHECK(before_retry->consecutive_retryable_failures == 3U);
    CHECK(before_retry->scheduled_retry_delay == std::chrono::milliseconds::zero());
    REQUIRE(after_retry.has_value());
    CHECK(after_retry->write_in_flight);
    CHECK(after_retry->retry_requested);
    CHECK(after_retry->consecutive_retryable_failures == 3U);
    CHECK(after_retry->scheduled_retry_delay == std::chrono::milliseconds::zero());
    auto pending = scope.engine().progress(*backend);
    REQUIRE(pending.has_value());
    CHECK(pending->retry_requested);
    CHECK(pending->delivery == delivery_state::pending);
    CHECK(pending->consecutive_retryable_failures == 0U);
    CHECK(pending->scheduled_retry_delay == std::chrono::milliseconds::zero());
    REQUIRE(pending->last_completed_attempt.has_value());
    const auto previous_attempt = pending->last_completed_attempt.value_or(completed_attempt{});
    CHECK(previous_attempt.id == attempt_id{1U});
    CHECK(previous_attempt.output == sink_state->call(0U).output);
    CHECK(scope.engine().drive_one(*lane));

    CHECK(sink_state->call_count() == 2U);
    CHECK(sink_state->call(0U).publication == sink_state->call(1U).publication);
    CHECK(sink_state->call(0U).output == sink_state->call(1U).output);
    CHECK(sink_state->call(1U).attempt == attempt_id{2U});
    auto completed = scope.engine().progress(*backend);
    REQUIRE(completed.has_value());
    CHECK(!completed->retry_requested);
    CHECK(completed->delivery == delivery_state::setter_accepted);
}

TEST_CASE("output.retry_during_inflight_failure_is_not_replaced_by_backoff_or_blocked_state",
    "[component][output][execution]") {
    for (const auto status :
        {native_status::retryable_failure, native_status::unsupported, native_status::failed}) {
        authority_fixture fixture{};
        execution_engine_scope scope{fixture.owner};
        const auto sink_state = std::make_shared<controlled_native_state>();
        sink_state->block_next_write();
        sink_state->set_next_result(native_result{.status = status});
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        REQUIRE(
            execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 3U));
        auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());

        std::jthread drive_thread(
            [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
        const bool entered = sink_state->wait_for_blocked_write();
        bool retry_accepted = false;
        if (entered) {
            retry_accepted = scope.engine().request_retry(*backend).has_value();
        }
        sink_state->release_write();
        drive_thread.join();
        CHECK(entered);
        CHECK(retry_accepted);

        auto pending = scope.engine().progress(*backend);
        REQUIRE(pending.has_value());
        CHECK(pending->retry_requested);
        CHECK(pending->delivery == delivery_state::pending);
        CHECK(pending->consecutive_retryable_failures ==
              (status == native_status::retryable_failure ? 4U : 0U));
        CHECK(pending->scheduled_retry_delay == std::chrono::milliseconds::zero());
        REQUIRE(pending->last_completed_attempt.has_value());
        const auto completed_attempt_value =
            pending->last_completed_attempt.value_or(completed_attempt{});
        CHECK(completed_attempt_value.result.status == status);
        CHECK(completed_attempt_value.output == sink_state->call(0U).output);
        CHECK(scope.engine().drive_one(*lane));
        CHECK(sink_state->call_count() == 2U);
        CHECK(sink_state->call(1U).attempt == attempt_id{2U});
    }
}

TEST_CASE("output.retry_during_inflight_disconnection_retires_backend_after_new_publication",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    sink_state->set_next_result(native_result{.status = native_status::disconnected});
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 3U));
    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());

    std::jthread drive_thread(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const bool entered = sink_state->wait_for_blocked_write();
    bool retry_accepted = false;
    bool mutation_accepted = false;
    bool publication_accepted = false;
    if (entered) {
        retry_accepted = scope.engine().request_retry(*backend).has_value();
        auto context = fixture.owner.open(fixture.connection.target);
        if (context.has_value()) {
            const auto request = inputnext::test::output::rumble_request({.channels = rumble_mask,
                .sequence = 1U,
                .ttl = std::chrono::seconds{5},
                .low = 0.4F,
                .high = 0.7F});
            mutation_accepted = fixture.owner.submit(*context, request).has_value();
            auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
            if (snapshot.has_value()) {
                publication_accepted = scope.engine().publish(*backend, *snapshot).has_value();
            }
        }
    }
    sink_state->release_write();
    drive_thread.join();

    CHECK(entered);
    CHECK(retry_accepted);
    CHECK(mutation_accepted);
    CHECK(publication_accepted);
    CHECK(sink_state->call_count() == 1U);

    auto disconnected = scope.engine().progress(*backend);
    REQUIRE(disconnected.has_value());
    CHECK(disconnected->phase == backend_phase::retiring);
    CHECK(disconnected->consecutive_retryable_failures == 0U);
    CHECK(disconnected->scheduled_retry_delay == std::chrono::milliseconds::zero());
    CHECK(disconnected->observed_publication.value == 2U);
    CHECK(!disconnected->retry_requested);
    CHECK(!disconnected->write_in_flight);
    REQUIRE(disconnected->last_completed_attempt.has_value());
    const auto completed = disconnected->last_completed_attempt.value_or(completed_attempt{});
    CHECK(completed.publication.value == 1U);
    CHECK(completed.result.status == native_status::disconnected);
    CHECK(completed.output == sink_state->call(0U).output);

    auto receipt = scope.engine().retire(*backend);
    REQUIRE(receipt.has_value());
    CHECK(receipt->reason == retirement_reason::disconnected);
    CHECK(scope.engine().drive_one(*lane));
    auto drained = scope.engine().progress(*backend);
    REQUIRE(drained.has_value());
    CHECK(drained->phase == backend_phase::drained);
    CHECK(sink_state->destruction_count() == 1U);
    CHECK(sink_state->call_count() == 1U);
    CHECK(scope.engine().acknowledge_retirement(*receipt).has_value());
    auto stale_retry = scope.engine().request_retry(*backend);
    CHECK(!stale_retry.has_value());
    if (!stale_retry.has_value()) {
        CHECK(stale_retry.error() == execution_error::stale_backend);
    }
}

TEST_CASE("output.late_disconnection_cannot_retire_replacement_backend",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto first_state = std::make_shared<controlled_native_state>();
    first_state->block_next_write();
    first_state->set_next_result(native_result{.status = native_status::disconnected});
    std::unique_ptr<native_sink> first_sink = std::make_unique<controlled_native_sink>(first_state);
    auto first = scope.register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first.has_value());
    auto lane = scope.engine().lane_for(*first);
    REQUIRE(lane.has_value());

    std::jthread drive_thread(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const bool entered = first_state->wait_for_blocked_write();
    bool retry_accepted = false;
    const auto replacement_state = std::make_shared<controlled_native_state>();
    std::optional<backend_handle> replacement;
    if (entered) {
        retry_accepted = scope.engine().request_retry(*first).has_value();
        std::unique_ptr<native_sink> replacement_sink =
            std::make_unique<controlled_native_sink>(replacement_state);
        auto committed = scope.register_backend(fixture.connection.target,
            replacement_sink,
            rumble_mask,
            *first);
        if (committed.has_value()) {
            replacement = *committed;
        }
    }
    first_state->release_write();
    drive_thread.join();

    CHECK(entered);
    CHECK(retry_accepted);
    REQUIRE(replacement.has_value());
    const auto replacement_handle = replacement.value_or(backend_handle{});
    CHECK(first_state->call_count() == 1U);
    CHECK(replacement_state->call_count() == 0U);

    auto old_progress = scope.engine().progress(*first);
    auto current_progress = scope.engine().progress(replacement_handle);
    REQUIRE(old_progress.has_value());
    REQUIRE(current_progress.has_value());
    CHECK(old_progress->phase == backend_phase::retiring);
    CHECK(current_progress->phase == backend_phase::current);
    REQUIRE(old_progress->last_completed_attempt.has_value());
    const auto old_completion = old_progress->last_completed_attempt.value_or(completed_attempt{});
    CHECK(old_completion.result.status == native_status::disconnected);
    auto receipt = scope.engine().retire(*first);
    REQUIRE(receipt.has_value());
    CHECK(receipt->reason == retirement_reason::replaced);

    CHECK(scope.engine().drive_one(*lane));
    old_progress = scope.engine().progress(*first);
    current_progress = scope.engine().progress(replacement_handle);
    REQUIRE(old_progress.has_value());
    REQUIRE(current_progress.has_value());
    CHECK(old_progress->phase == backend_phase::drained);
    CHECK(current_progress->phase == backend_phase::current);
    CHECK(first_state->destruction_count() == 1U);
    CHECK(first_state->call_count() == 1U);
    CHECK(replacement_state->call_count() == 0U);
    CHECK(scope.engine().acknowledge_retirement(*receipt).has_value());
}

} // namespace
