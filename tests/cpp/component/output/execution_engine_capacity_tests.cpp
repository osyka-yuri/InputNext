#include "output/authority_fixture.hpp"

#include "execution_engine_scope.hpp"
#include "execution_engine_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution/engine.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;
using inputnext::test::output::registry_source;

TEST_CASE("output.execution_resident_backend_capacity_retains_cleanup_and_preserves_current",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::vector<backend_handle> handles(execution_backend_capacity);

    for (std::size_t index = 0U; index < handles.size(); ++index) {
        std::unique_ptr<native_sink> candidate =
            std::make_unique<controlled_native_sink>(sink_state);
        const auto expected = index == 0U ? std::optional<backend_handle>{}
                                          : std::optional<backend_handle>{handles[index - 1U]};
        auto registered =
            scope.register_backend(fixture.connection.target, candidate, rumble_mask, expected);
        REQUIRE(registered.has_value());
        CHECK(candidate == nullptr);
        handles[index] = *registered;
    }

    std::unique_ptr<native_sink> overflow_candidate =
        std::make_unique<controlled_native_sink>(sink_state);
    auto overflow = scope.engine().reserve_backend(fixture.connection.target,
        overflow_candidate,
        rumble_mask,
        handles.back());
    CHECK(!overflow.has_value());
    if (!overflow.has_value()) {
        CHECK(overflow.error() == execution_error::capacity_exceeded);
    }
    CHECK(overflow_candidate != nullptr);
    auto current = scope.engine().progress(handles.back());
    REQUIRE(current.has_value());
    CHECK(current->phase == backend_phase::current);

    std::vector<retirement_receipt> receipts(execution_backend_capacity);
    for (std::size_t index = 0U; index < handles.size(); ++index) {
        auto receipt = scope.engine().retire(handles[index]);
        REQUIRE(receipt.has_value());
        receipts[index] = *receipt;
    }
    auto lane = scope.engine().lane_for(handles.back());
    REQUIRE(lane.has_value());
    for (std::size_t index = 0U; index < handles.size(); ++index) {
        CHECK(scope.engine().drive_one(*lane));
    }
    CHECK(!scope.engine().drive_one(*lane));
    CHECK(sink_state->destruction_count() == execution_backend_capacity);

    for (const auto& receipt : receipts) {
        auto progress = scope.engine().progress(receipt.backend);
        REQUIRE(progress.has_value());
        CHECK(progress->phase == backend_phase::drained);
        CHECK(scope.engine().acknowledge_retirement(receipt).has_value());
    }
    CHECK(scope.engine().worker_should_exit(*lane));
    CHECK(scope.worker_joined(*lane).has_value());
}

TEST_CASE("output.execution_identity_exhaustion_blocks_admission_and_keeps_cleanup_available",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};

    SECTION("backend identity") {
        execution_engine_test_access::seed_next_backend_id(scope.engine(), 0U);
        const auto sink_state = std::make_shared<controlled_native_state>();
        std::unique_ptr<native_sink> candidate =
            std::make_unique<controlled_native_sink>(sink_state);
        auto reserved =
            scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
        CHECK(!reserved.has_value());
        if (!reserved.has_value()) {
            CHECK(reserved.error() == execution_error::identity_exhausted);
        }
        CHECK(candidate != nullptr);
    }

    SECTION("lane generation identity") {
        execution_engine_test_access::seed_next_lane_generation(scope.engine(), 0U);
        std::unique_ptr<native_sink> candidate =
            std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());
        auto reserved =
            scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
        CHECK(!reserved.has_value());
        if (!reserved.has_value()) {
            CHECK(reserved.error() == execution_error::identity_exhausted);
        }
        CHECK(candidate != nullptr);
        CHECK(!scope.engine().has_current_backends());
    }

    SECTION("publication identity") {
        const auto sink_state = std::make_shared<controlled_native_state>();
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        const auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(execution_engine_test_access::seed_backend_ids(scope.engine(),
            *backend,
            {.next_publication = 0U, .next_attempt = 1U}));

        auto context = fixture.owner.open(fixture.connection.target);
        REQUIRE(context.has_value());
        const auto request = inputnext::test::output::rumble_request({.channels = rumble_mask,
            .sequence = 1U,
            .ttl = std::chrono::seconds{5},
            .low = 0.2F,
            .high = 0.3F});
        REQUIRE(fixture.owner.submit(*context, request).has_value());
        auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
        REQUIRE(snapshot.has_value());
        auto published = scope.engine().publish(*backend, *snapshot);
        CHECK(!published.has_value());
        if (!published.has_value()) {
            CHECK(published.error() == execution_error::identity_exhausted);
        }
        CHECK(!scope.engine().request_retry(*backend).has_value());
        CHECK(sink_state->call_count() == 0U);
    }

    SECTION("attempt identity") {
        const auto sink_state = std::make_shared<controlled_native_state>();
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(execution_engine_test_access::seed_backend_ids(scope.engine(),
            *backend,
            {.next_publication = 2U, .next_attempt = 0U}));

        CHECK(scope.engine().drive_one(*lane));
        CHECK(sink_state->call_count() == 0U);
        auto progress = scope.engine().progress(*backend);
        REQUIRE(progress.has_value());
        CHECK(progress->delivery == delivery_state::identity_exhausted);
        CHECK(progress->phase == backend_phase::retiring);
        CHECK(!progress->last_attempt.has_value());
    }

    SECTION("last attempt serial is delivered before the backend retires") {
        const auto sink_state = std::make_shared<controlled_native_state>();
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        auto backend = scope.register_backend(fixture.connection.target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        CHECK(execution_engine_test_access::seed_backend_ids(scope.engine(),
            *backend,
            {.next_publication = 2U, .next_attempt = std::numeric_limits<std::uint64_t>::max()}));

        CHECK(scope.engine().drive_one(*lane));
        const auto command = sink_state->call(0U);
        auto progress = scope.engine().progress(*backend);
        REQUIRE(progress.has_value());
        CHECK(command.attempt.value == std::numeric_limits<std::uint64_t>::max());
        CHECK(progress->phase == backend_phase::retiring);
        CHECK(progress->delivery == delivery_state::identity_exhausted);
        const auto completed = progress->last_completed_attempt.value_or(completed_attempt{});
        CHECK(completed.id == command.attempt);
        CHECK(!scope.engine().request_retry(*backend).has_value());
    }
}

TEST_CASE("output.execution_lane_capacity_retains_closing_workers_until_join",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto sink_state = std::make_shared<controlled_native_state>();
    std::array<lane_token, execution_lane_capacity> lanes{};

    for (auto& lane_token_value : lanes) {
        auto source = std::unique_ptr<source_lifetime>{std::make_unique<registry_source>()};
        auto connection = fixture.registry.admit(source);
        REQUIRE(connection.has_value());
        std::unique_ptr<native_sink> sink = std::make_unique<controlled_native_sink>(sink_state);
        auto backend = scope.register_backend(connection->target, sink, rumble_mask);
        REQUIRE(backend.has_value());
        auto lane = scope.engine().lane_for(*backend);
        REQUIRE(lane.has_value());
        lane_token_value = *lane;
        auto receipt = scope.engine().retire(*backend);
        REQUIRE(receipt.has_value());
        CHECK(scope.engine().drive_one(*lane));
        CHECK(scope.engine().acknowledge_retirement(*receipt).has_value());
        CHECK(scope.engine().worker_should_exit(*lane));

        const auto registry_retired = fixture.registry.retire(connection->target);
        REQUIRE(registry_retired.has_value());
        CHECK(fixture.registry.drain_retired() == 1U);
    }

    std::unique_ptr<native_sink> overflow_candidate =
        std::make_unique<controlled_native_sink>(sink_state);
    auto source = std::unique_ptr<source_lifetime>{std::make_unique<registry_source>()};
    auto connection = fixture.registry.admit(source);
    REQUIRE(connection.has_value());
    auto overflow =
        scope.engine().reserve_backend(connection->target, overflow_candidate, rumble_mask);
    CHECK(!overflow.has_value());
    if (!overflow.has_value()) {
        CHECK(overflow.error() == execution_error::capacity_exceeded);
    }
    CHECK(overflow_candidate != nullptr);
    const auto registry_retired = fixture.registry.retire(connection->target);
    REQUIRE(registry_retired.has_value());
    CHECK(fixture.registry.drain_retired() == 1U);

    for (const auto lane : lanes) {
        CHECK(scope.worker_joined(lane).has_value());
    }
}

} // namespace
