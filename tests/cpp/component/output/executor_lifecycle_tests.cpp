#include "output/authority_fixture.hpp"

#include "executor_test_access.hpp"
#include "executor_test_support.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <future>
#include <memory>
#include <thread>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::output::detail::executor_test_access;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::release_native_controls;
using inputnext::test::output::reset_thread_start_control;
using inputnext::test::output::start_executor;

TEST_CASE("output.executor_creation_failure_releases_authority_attachment",
    "[component][output][executor][lifecycle]") {
    authority_fixture fixture{};
    reset_thread_start_control reset_control{};
    executor_test_access::fail_next_coordinator_start();

    auto failed = executor::create(fixture.owner);
    CHECK(!failed.has_value());
    if (!failed.has_value()) {
        CHECK(failed.error() == execution_error::resources);
    }

    auto recovered = executor::create(fixture.owner);
    REQUIRE(recovered.has_value());
    (*recovered)->shutdown();
}

TEST_CASE("output.executor_attaches_once_and_recreation_uses_a_new_owner",
    "[component][output][executor][lifecycle]") {
    authority_fixture fixture{};
    const auto first = start_executor(fixture.owner);

    auto competing = executor::create(fixture.owner);
    CHECK(!competing.has_value());
    if (!competing.has_value()) {
        CHECK(competing.error() == execution_error::already_attached);
    }

    const auto first_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> first_sink = std::make_unique<controlled_native_sink>(first_state);
    auto first_backend =
        first->register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first_backend.has_value());
    CHECK(first_sink == nullptr);

    first->shutdown();
    CHECK(first_state->destruction_count() == 1U);
    auto stopped_progress = first->progress(*first_backend);
    CHECK(!stopped_progress.has_value());
    if (!stopped_progress.has_value()) {
        CHECK(stopped_progress.error() == execution_error::not_running);
    }

    const auto second = start_executor(fixture.owner);
    const auto second_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> second_sink =
        std::make_unique<controlled_native_sink>(second_state);
    auto second_backend =
        second->register_backend(fixture.connection.target, second_sink, rumble_mask);
    REQUIRE(second_backend.has_value());
    CHECK(second_backend->incarnation.owner != first_backend->incarnation.owner);
    auto foreign_progress = second->progress(*first_backend);
    CHECK(!foreign_progress.has_value());
    if (!foreign_progress.has_value()) {
        CHECK(foreign_progress.error() == execution_error::stale_backend);
    }
    second->shutdown();
}

TEST_CASE("output.concurrent_shutdown_waits_for_native_call_and_sink_destruction",
    "[component][output][executor][lifecycle]") {
    authority_fixture fixture{};
    auto running = start_executor(fixture.owner);
    const auto state = std::make_shared<controlled_native_state>();
    state->block_next_write();
    state->block_destruction();
    release_native_controls release_before_threads{state};

    std::unique_ptr<native_sink> candidate = std::make_unique<controlled_native_sink>(state);
    auto backend = running->register_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(backend.has_value());
    const bool write_blocked = state->wait_for_blocked_write();
    REQUIRE(write_blocked);
    const auto receipt = running->retire(*backend);
    REQUIRE(receipt.has_value());

    std::promise<void> first_done;
    std::promise<void> second_done;
    const auto first_future = first_done.get_future();
    const auto second_future = second_done.get_future();
    std::jthread first_shutdown{[&] {
        running->shutdown();
        first_done.set_value();
    }};
    std::jthread second_shutdown{[&] {
        running->shutdown();
        second_done.set_value();
    }};
    release_native_controls release_before_join{state};

    const bool first_waited_for_write =
        first_future.wait_for(std::chrono::milliseconds{30}) != std::future_status::ready;
    const bool second_waited_for_write =
        second_future.wait_for(std::chrono::milliseconds{30}) != std::future_status::ready;
    state->release_write();
    const bool destructor_entered = state->wait_for_destruction();
    const bool shutdown_waited_for_destructor =
        first_future.wait_for(std::chrono::milliseconds{30}) != std::future_status::ready &&
        second_future.wait_for(std::chrono::milliseconds{30}) != std::future_status::ready;
    state->release_destruction();

    const bool first_finished =
        first_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    const bool second_finished =
        second_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    first_shutdown.join();
    second_shutdown.join();

    CHECK(first_waited_for_write);
    CHECK(second_waited_for_write);
    CHECK(destructor_entered);
    CHECK(shutdown_waited_for_destructor);
    CHECK(first_finished);
    CHECK(second_finished);
    CHECK(state->destruction_count() == 1U);
}

} // namespace
