#include "output/authority_fixture.hpp"

#include "execution_engine_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

class release_write_guard final {
public:
    explicit release_write_guard(controlled_native_state& state) noexcept : state_(state) {}

    ~release_write_guard() noexcept { state_.release_write(); }

    release_write_guard(const release_write_guard&) = delete;
    release_write_guard& operator=(const release_write_guard&) = delete;
    release_write_guard(release_write_guard&&) = delete;
    release_write_guard& operator=(release_write_guard&&) = delete;

private:
    controlled_native_state& state_;
};

TEST_CASE("output.execution_backoff_preserves_count_when_fresh_qualification_retires_backend",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 3U));

    std::jthread writer(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const release_write_guard release{*sink_state};
    const bool entered = sink_state->wait_for_blocked_write();
    bool connection_retired = false;
    if (entered) {
        connection_retired = fixture.registry.retire(fixture.connection.target).has_value();
    }
    sink_state->release_write();
    writer.join();

    CHECK(entered);
    CHECK(connection_retired);
    auto retired = scope.engine().progress(*backend);
    REQUIRE(retired.has_value());
    CHECK(retired->phase == backend_phase::retiring);
    CHECK(retired->consecutive_retryable_failures == 3U);
    CHECK(retired->scheduled_retry_delay == 0ms);

    const auto receipt = scope.engine().retire(*backend);
    REQUIRE(receipt.has_value());
    REQUIRE(scope.engine().drive_one(*lane));
    const auto drained = scope.engine().progress(*backend);
    REQUIRE(drained.has_value());
    CHECK(drained->phase == backend_phase::drained);
    CHECK(drained->consecutive_retryable_failures == 3U);
    CHECK(drained->scheduled_retry_delay == 0ms);
    REQUIRE(scope.engine().acknowledge_retirement(*receipt).has_value());
}

TEST_CASE("output.execution_backoff_preserves_count_when_foreground_qualification_defers_refresh",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto context = fixture.owner.open(fixture.connection.target);
    REQUIRE(context.has_value());
    const auto submitted = fixture.owner.submit(*context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.7F}));
    REQUIRE(submitted.has_value());
    const auto represented = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(represented.has_value());

    const auto sink_state = std::make_shared<controlled_native_state>();
    sink_state->block_next_write();
    std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
    const auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
    REQUIRE(backend.has_value());
    REQUIRE(scope.engine().publish(*backend, *represented).has_value());
    const auto lane = scope.engine().lane_for(*backend);
    REQUIRE(lane.has_value());
    REQUIRE(execution_engine_test_access::seed_retryable_failures(scope.engine(), *backend, 3U));

    std::jthread writer(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const release_write_guard release{*sink_state};
    const bool entered = sink_state->wait_for_blocked_write();
    if (entered) {
        fixture.focus_observer.stop();
    }
    sink_state->release_write();
    writer.join();

    CHECK(entered);
    const auto deferred = scope.engine().progress(*backend);
    REQUIRE(deferred.has_value());
    CHECK(deferred->phase == backend_phase::current);
    CHECK(deferred->delivery == delivery_state::pending);
    CHECK(!deferred->write_in_flight);
    CHECK(deferred->consecutive_retryable_failures == 3U);
    CHECK(deferred->scheduled_retry_delay == 0ms);
    CHECK(!scope.engine().drive_one(*lane));
    CHECK(sink_state->call_count() == 1U);

    const auto refreshed = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(refreshed.has_value());
    CHECK(!refreshed->rumble.has_value());
    REQUIRE(scope.engine().publish(*backend, *refreshed).has_value());
    const auto pending = scope.engine().progress(*backend);
    REQUIRE(pending.has_value());
    CHECK(pending->observed_publication == publication_id{3U});
    CHECK(pending->delivery == delivery_state::pending);
    CHECK(pending->consecutive_retryable_failures == 0U);
    CHECK(pending->scheduled_retry_delay == 0ms);

    REQUIRE(scope.engine().drive_one(*lane));
    const auto accepted = scope.engine().progress(*backend);
    REQUIRE(accepted.has_value());
    CHECK(accepted->delivery == delivery_state::setter_accepted);
    CHECK(accepted->consecutive_retryable_failures == 0U);
}

} // namespace
