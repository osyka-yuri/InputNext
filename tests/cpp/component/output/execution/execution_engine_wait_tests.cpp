#include "output/authority_fixture.hpp"

#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
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

TEST_CASE("output.execution_worker_waits_during_startup_until_initial_commit",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto reservation = scope.engine().reserve_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(reservation.has_value());
    scope.remember(reservation->lane);

    std::promise<void> entered;
    const auto entered_future = entered.get_future();
    std::atomic_bool worker_failed{};
    std::atomic_bool worker_exited{};
    std::jthread worker{
        [&scope, &entered, &worker_failed, &worker_exited, lane_token_value = reservation->lane](
            const std::stop_token& stop_token) {
            entered.set_value();
            while (scope.engine().wait_for_work(lane_token_value, stop_token)) {
                if (!scope.engine().drive_one(lane_token_value)) {
                    worker_failed.store(true);
                }
            }
            worker_exited.store(true);
        }};

    const auto entered_wait = entered_future.wait_for(std::chrono::seconds{1});
    const auto started = scope.engine().worker_started(reservation->lane);
    execution_result<backend_handle> committed =
        started.has_value() ? scope.engine().commit_backend(*reservation, sink)
                            : execution_result<backend_handle>{std::unexpected{started.error()}};
    if (!committed.has_value()) {
        const auto canceled = scope.engine().cancel_backend(*reservation);
        if (!canceled.has_value()) {
            scope.engine().close_all();
        }
    }

    const bool initial_write = committed.has_value() && sink_state->wait_for_write_count(1U);
    execution_result<retirement_receipt> receipt =
        committed.has_value()
            ? scope.engine().retire(*committed)
            : execution_result<retirement_receipt>{std::unexpected{execution_error::stale_backend}};
    bool destruction_observed = false;
    bool acknowledged = false;
    if (receipt.has_value()) {
        destruction_observed = sink_state->wait_for_destruction();
        if (!destruction_observed) {
            scope.engine().close_all();
            destruction_observed = sink_state->wait_for_destruction();
        }
    }
    scope.engine().close_all();
    if (worker.joinable()) {
        worker.join();
    }
    if (receipt.has_value()) {
        const auto progress = scope.engine().progress(receipt->backend);
        if (progress.has_value() && progress->phase == backend_phase::drained) {
            acknowledged = scope.engine().acknowledge_retirement(*receipt).has_value();
        }
    }
    const bool worker_joined = scope.worker_joined(reservation->lane).has_value();

    CHECK(entered_wait == std::future_status::ready);
    CHECK(started.has_value());
    CHECK(committed.has_value());
    CHECK(initial_write);
    CHECK(receipt.has_value());
    CHECK(destruction_observed);
    CHECK(acknowledged);
    CHECK(!worker_failed.load());
    CHECK(worker_exited.load());
    CHECK(worker_joined);
}

TEST_CASE("output.execution_cancel_wakes_worker_waiting_on_starting_lane",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    auto reservation =
        scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(reservation.has_value());
    scope.remember(reservation->lane);

    std::promise<void> entered;
    const auto entered_future = entered.get_future();
    std::atomic_bool wait_result{true};
    std::jthread worker{[&scope, &entered, &wait_result, lane_token_value = reservation->lane](
                            const std::stop_token& stop_token) {
        entered.set_value();
        wait_result.store(scope.engine().wait_for_work(lane_token_value, stop_token));
    }};

    const auto entered_wait = entered_future.wait_for(std::chrono::seconds{1});
    const auto canceled = scope.engine().cancel_backend(*reservation);
    worker.join();
    const auto joined = scope.worker_joined(reservation->lane);

    CHECK(entered_wait == std::future_status::ready);
    CHECK(canceled.has_value());
    CHECK(!wait_result.load());
    CHECK(joined.has_value());
    CHECK(candidate != nullptr);
}

TEST_CASE("output.execution_close_wakes_worker_waiting_on_starting_lane",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
    auto reservation =
        scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(reservation.has_value());
    scope.remember(reservation->lane);

    std::promise<void> entered;
    const auto entered_future = entered.get_future();
    std::atomic_bool wait_result{true};
    std::jthread worker{[&scope, &entered, &wait_result, lane_token_value = reservation->lane](
                            const std::stop_token& stop_token) {
        entered.set_value();
        wait_result.store(scope.engine().wait_for_work(lane_token_value, stop_token));
    }};

    const auto entered_wait = entered_future.wait_for(std::chrono::seconds{1});
    scope.engine().close_all();
    worker.join();
    const auto canceled = scope.engine().cancel_backend(*reservation);
    const auto joined = scope.worker_joined(reservation->lane);

    CHECK(entered_wait == std::future_status::ready);
    CHECK(!wait_result.load());
    CHECK(canceled.has_value());
    CHECK(joined.has_value());
    CHECK(candidate != nullptr);
}

TEST_CASE("output.execution_worker_drains_retired_sink_before_observing_stop",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    auto receipt = scope.engine().retire(*backend);
    REQUIRE(receipt.has_value());

    std::stop_source source;
    source.request_stop();
    const auto has_cleanup = scope.engine().wait_for_work(*lane, source.get_token());
    const auto cleanup_driven = has_cleanup && scope.engine().drive_one(*lane);
    const auto should_exit = !scope.engine().wait_for_work(*lane, source.get_token());
    const auto progress = scope.engine().progress(*backend);
    const bool drained = progress.has_value() && progress->phase == backend_phase::drained;
    const auto acknowledged = scope.engine().acknowledge_retirement(*receipt);
    const auto joined = scope.worker_joined(*lane);

    CHECK(has_cleanup);
    CHECK(cleanup_driven);
    CHECK(should_exit);
    CHECK(drained);
    CHECK(acknowledged.has_value());
    CHECK(joined.has_value());
}

} // namespace
