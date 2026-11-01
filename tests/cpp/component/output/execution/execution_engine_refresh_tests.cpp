#include "output/authority_fixture.hpp"

#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
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

TEST_CASE("output.execution_refresh_wakes_idle_coordinator_without_lost_request",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    std::promise<void> entered;
    const auto entered_future = entered.get_future();
    std::atomic_bool refreshed{};

    std::jthread waiter{[&scope, &entered, &refreshed](const std::stop_token& stop_token) {
        entered.set_value();
        refreshed.store(scope.engine().wait_for_refresh(stop_token));
    }};

    const auto started = entered_future.wait_for(std::chrono::seconds{1});
    CHECK(started == std::future_status::ready);
    scope.engine().request_drive();
    waiter.join();
    CHECK(refreshed.load());
    CHECK(!scope.engine().has_current_backends());

    std::promise<void> idle_entered;
    const auto idle_entered_future = idle_entered.get_future();
    std::atomic_bool stopped_result{true};
    std::jthread idle_waiter{
        [&scope, &idle_entered, &stopped_result](const std::stop_token& stop_token) {
            idle_entered.set_value();
            stopped_result.store(scope.engine().wait_for_refresh(stop_token));
        }};
    const auto idle_started = idle_entered_future.wait_for(std::chrono::seconds{1});
    CHECK(idle_started == std::future_status::ready);
    idle_waiter.request_stop();
    idle_waiter.join();
    CHECK(!stopped_result.load());

    std::promise<void> shutdown_entered;
    const auto shutdown_entered_future = shutdown_entered.get_future();
    std::atomic_bool shutdown_result{true};
    std::jthread shutdown_waiter{
        [&scope, &shutdown_entered, &shutdown_result](const std::stop_token& stop_token) {
            shutdown_entered.set_value();
            shutdown_result.store(scope.engine().wait_for_refresh(stop_token));
        }};
    const auto shutdown_started = shutdown_entered_future.wait_for(std::chrono::seconds{1});
    CHECK(shutdown_started == std::future_status::ready);
    scope.engine().close_all();
    shutdown_waiter.join();
    CHECK(!shutdown_result.load());
}

TEST_CASE("output.execution_refresh_enumerates_current_backends_and_runs_periodically",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());

    CHECK(scope.engine().has_current_backends());
    auto current = scope.engine().current_backends();
    REQUIRE(current.has_value());
    CHECK(current->count == 1U);
    CHECK(current->handles[0U] == *backend);
    CHECK(scope.engine().wait_for_refresh(std::stop_token{}));
    CHECK(scope.engine().wait_for_refresh(std::stop_token{}));

    const auto receipt = scope.engine().retire(*backend);
    REQUIRE(receipt.has_value());
    CHECK(scope.engine().wait_for_refresh(std::stop_token{}));
    CHECK(!scope.engine().has_current_backends());
    auto empty = scope.engine().current_backends();
    REQUIRE(empty.has_value());
    CHECK(empty->count == 0U);
}

} // namespace
