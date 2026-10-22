#include "output/authority_fixture.hpp"

#include "execution_engine_scope.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <memory>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;
using inputnext::test::output::registry_source;

TEST_CASE("output.execution_replacement_waits_for_old_write_and_sink_cleanup",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto old_state = std::make_shared<controlled_native_state>();
    old_state->block_next_write();
    std::unique_ptr<native_sink> old_sink = std::make_unique<controlled_native_sink>(old_state);
    auto old_backend = scope.register_backend(fixture.connection.target, old_sink, rumble_mask);
    REQUIRE(old_backend.has_value());
    auto lane = scope.engine().lane_for(*old_backend);
    REQUIRE(lane.has_value());

    std::jthread old_drive(
        [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const bool write_entered = old_state->wait_for_blocked_write();
    std::unique_ptr<source_lifetime> source = std::make_unique<registry_source>();
    auto connection = fixture.registry.replace(fixture.connection.target, source);
    std::shared_ptr<controlled_native_state> const new_state =
        std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> new_sink = std::make_unique<controlled_native_sink>(new_state);
    execution_result<backend_handle> new_backend = std::unexpected{execution_error::resources};
    if (connection.has_value()) {
        new_backend =
            scope.register_backend(connection->target, new_sink, rumble_mask, *old_backend);
    }
    auto old_receipt = scope.engine().retire(*old_backend);
    const auto pending_ack =
        old_receipt.has_value()
            ? scope.engine().acknowledge_retirement(*old_receipt)
            : execution_result<void>{std::unexpected{execution_error::resources}};

    std::jthread new_drive;
    if (new_backend.has_value()) {
        new_drive = std::jthread(
            [&scope, token = *lane] { static_cast<void>(scope.engine().drive_one(token)); });
    }
    const auto calls_before_release = new_state->call_count();
    old_state->release_write();
    old_drive.join();
    if (new_drive.joinable()) {
        new_drive.join();
    }

    CHECK(write_entered);
    CHECK(connection.has_value());
    CHECK(new_backend.has_value());
    CHECK(!pending_ack.has_value());
    if (!pending_ack.has_value()) {
        CHECK(pending_ack.error() == execution_error::pending);
    }
    CHECK(calls_before_release == 0U);
    CHECK(old_state->call_count() == 1U);
    auto old_progress = scope.engine().progress(*old_backend);
    REQUIRE(old_progress.has_value());
    CHECK(old_progress->phase == backend_phase::drained);
    CHECK(new_state->destruction_count() == 0U);

    CHECK(scope.engine().drive_one(*lane));
    CHECK(new_state->call_count() == 1U);
    auto new_progress = scope.engine().progress(*new_backend);
    REQUIRE(new_progress.has_value());
    CHECK(new_progress->delivery == delivery_state::setter_accepted);
}

TEST_CASE("output.execution_drained_ack_is_exact_after_backend_slot_reuse",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto first_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> first_sink = std::make_unique<controlled_native_sink>(first_state);
    auto first = scope.register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first.has_value());
    auto lane = scope.engine().lane_for(*first);
    REQUIRE(lane.has_value());
    auto receipt = scope.engine().retire(*first);
    REQUIRE(receipt.has_value());
    CHECK(scope.engine().drive_one(*lane));
    CHECK(scope.engine().acknowledge_retirement(*receipt).has_value());
    CHECK(scope.engine().worker_should_exit(*lane));
    CHECK(scope.worker_joined(*lane).has_value());

    const auto second_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> second_sink =
        std::make_unique<controlled_native_sink>(second_state);
    auto second = scope.register_backend(fixture.connection.target, second_sink, rumble_mask);
    REQUIRE(second.has_value());
    CHECK(second->incarnation.id != first->incarnation.id);
    auto stale_ack = scope.engine().acknowledge_retirement(*receipt);
    CHECK(!stale_ack.has_value());
    if (!stale_ack.has_value()) {
        CHECK(stale_ack.error() == execution_error::stale_backend);
    }
    auto progress = scope.engine().progress(*second);
    REQUIRE(progress.has_value());
    CHECK(progress->phase == backend_phase::current);
}

TEST_CASE("output.execution_blocked_destructor_keeps_backend_cleaning_and_other_lane_progresses",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto blocked_state = std::make_shared<controlled_native_state>();
    blocked_state->block_destruction();
    std::unique_ptr<native_sink> blocked_sink =
        std::make_unique<controlled_native_sink>(blocked_state);
    auto blocked_backend =
        scope.register_backend(fixture.connection.target, blocked_sink, rumble_mask);
    REQUIRE(blocked_backend.has_value());
    auto blocked_lane = scope.engine().lane_for(*blocked_backend);
    REQUIRE(blocked_lane.has_value());
    auto receipt = scope.engine().retire(*blocked_backend);
    REQUIRE(receipt.has_value());

    std::jthread cleanup(
        [&scope, token = *blocked_lane] { static_cast<void>(scope.engine().drive_one(token)); });
    const bool destructor_entered = blocked_state->wait_for_destruction();
    auto cleaning_progress = scope.engine().progress(*blocked_backend);
    auto pending_ack = scope.engine().acknowledge_retirement(*receipt);

    auto second_connection_source = std::make_unique<registry_source>();
    std::unique_ptr<source_lifetime> second_source = std::move(second_connection_source);
    auto second_connection = fixture.registry.admit(second_source);
    std::shared_ptr<controlled_native_state> const second_state =
        std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> second_sink =
        std::make_unique<controlled_native_sink>(second_state);
    execution_result<backend_handle> second_backend = std::unexpected{execution_error::resources};
    if (second_connection.has_value()) {
        second_backend =
            scope.register_backend(second_connection->target, second_sink, rumble_mask);
    }
    bool second_lane_drove = false;
    if (second_backend.has_value()) {
        auto second_lane = scope.engine().lane_for(*second_backend);
        if (second_lane.has_value()) {
            second_lane_drove = scope.engine().drive_one(*second_lane);
        }
    }

    blocked_state->release_destruction();
    cleanup.join();

    CHECK(destructor_entered);
    CHECK(cleaning_progress.has_value());
    if (!cleaning_progress.has_value()) {
        return;
    }
    CHECK(cleaning_progress->phase == backend_phase::cleaning);
    CHECK(!pending_ack.has_value());
    if (!pending_ack.has_value()) {
        CHECK(pending_ack.error() == execution_error::pending);
    }
    CHECK(second_connection.has_value());
    CHECK(second_backend.has_value());
    CHECK(second_lane_drove);
    CHECK(second_state->call_count() == 1U);
    CHECK(blocked_state->destruction_count() == 1U);
    auto drained = scope.engine().progress(*blocked_backend);
    REQUIRE(drained.has_value());
    CHECK(drained->phase == backend_phase::drained);
    CHECK(scope.engine().acknowledge_retirement(*receipt).has_value());
}

} // namespace
