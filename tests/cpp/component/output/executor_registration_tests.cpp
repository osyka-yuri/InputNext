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
#include <optional>
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

TEST_CASE("output.executor_worker_start_failure_preserves_candidate_and_releases_lane",
    "[component][output][executor][registration]") {
    authority_fixture fixture{};
    const auto running = start_executor(fixture.owner);
    reset_thread_start_control reset_control{};
    executor_test_access::fail_next_delivery_start();

    const auto failed_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> failed_candidate =
        std::make_unique<controlled_native_sink>(failed_state);
    auto failed =
        running->register_backend(fixture.connection.target, failed_candidate, rumble_mask);

    CHECK(!failed.has_value());
    if (!failed.has_value()) {
        CHECK(failed.error() == execution_error::resources);
    }
    CHECK(failed_candidate != nullptr);
    CHECK(failed_state->destruction_count() == 0U);

    const auto recovered_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> recovered_candidate =
        std::make_unique<controlled_native_sink>(recovered_state);
    const auto recovered =
        running->register_backend(fixture.connection.target, recovered_candidate, rumble_mask);
    REQUIRE(recovered.has_value());
    CHECK(recovered_candidate == nullptr);
    CHECK(recovered_state->wait_for_write_count(1U));
    running->shutdown();
    CHECK(recovered_state->destruction_count() == 1U);
    CHECK(failed_state->destruction_count() == 0U);
}

TEST_CASE("output.executor_replacement_waits_for_old_sink_destruction_on_its_lane",
    "[component][output][executor][registration]") {
    authority_fixture fixture{};
    const auto running = start_executor(fixture.owner);
    const auto old_state = std::make_shared<controlled_native_state>();
    old_state->block_destruction();
    release_native_controls release_controls{old_state};
    std::unique_ptr<native_sink> old_candidate =
        std::make_unique<controlled_native_sink>(old_state);
    auto old_backend =
        running->register_backend(fixture.connection.target, old_candidate, rumble_mask);
    REQUIRE(old_backend.has_value());
    REQUIRE(old_state->wait_for_write_count(1U));

    const auto new_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> new_candidate =
        std::make_unique<controlled_native_sink>(new_state);
    const auto new_backend = running->replace_backend(*old_backend,
        fixture.connection.target,
        new_candidate,
        rumble_mask);
    REQUIRE(new_backend.has_value());
    CHECK(new_candidate == nullptr);
    const bool old_destructor_entered = old_state->wait_for_destruction();
    const auto writes_before_release = new_state->call_count();
    old_state->release_destruction();
    const bool new_write_followed_cleanup = new_state->wait_for_write_count(1U);

    CHECK(old_destructor_entered);
    CHECK(writes_before_release == 0U);
    CHECK(new_write_followed_cleanup);
    CHECK(old_state->destruction_count() == 1U);
    running->shutdown();
}

TEST_CASE("output.executor_registration_racing_shutdown_rolls_back_without_transfer",
    "[component][output][executor][registration]") {
    authority_fixture fixture{};
    auto running = start_executor(fixture.owner);
    reset_thread_start_control reset_control{};
    executor_test_access::pause_next_delivery_start();

    const auto candidate_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(candidate_state);
    std::optional<execution_result<backend_handle>> registration;
    std::promise<void> registration_done;
    const auto registration_future = registration_done.get_future();
    std::jthread registering{[&] {
        registration.emplace(
            running->register_backend(fixture.connection.target, candidate, rumble_mask));
        registration_done.set_value();
    }};

    const bool reached_start_gate = executor_test_access::wait_for_paused_delivery_start();
    if (!reached_start_gate) {
        executor_test_access::release_paused_start();
        const bool completed =
            registration_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
        registering.join();
        CHECK(reached_start_gate);
        CHECK(completed);
        return;
    }

    std::promise<void> shutdown_done;
    const auto shutdown_future = shutdown_done.get_future();
    std::jthread shutting_down{[&] {
        running->shutdown();
        shutdown_done.set_value();
    }};
    release_native_controls release_controls{candidate_state};

    const bool engine_closed = executor_test_access::wait_for_engine_closed(*running);
    executor_test_access::release_paused_start();
    const bool registration_completed =
        registration_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    const bool shutdown_completed =
        shutdown_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    registering.join();
    shutting_down.join();

    CHECK(engine_closed);
    CHECK(registration_completed);
    CHECK(shutdown_completed);
    if (!registration.has_value()) {
        FAIL("registration thread did not return a result");
        return;
    }
    const auto& registration_result = registration.value();
    CHECK(!registration_result.has_value());
    if (!registration_result.has_value()) {
        CHECK(registration_result.error() == execution_error::not_running);
    }
    CHECK(candidate != nullptr);
    CHECK(candidate_state->destruction_count() == 0U);

    auto recreated = executor::create(fixture.owner);
    CHECK(recreated.has_value());
    if (recreated.has_value()) {
        (*recreated)->shutdown();
    }
}

} // namespace
