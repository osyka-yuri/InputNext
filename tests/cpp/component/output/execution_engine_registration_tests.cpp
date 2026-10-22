#include "output/authority_fixture.hpp"

#include "execution_engine_scope.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

TEST_CASE("output.execution_registration_rejects_occupied_and_wrong_destination",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto first_sink_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> first_sink =
        std::make_unique<controlled_native_sink>(first_sink_state);
    auto first = scope.register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first.has_value());
    CHECK(first_sink == nullptr);

    const auto conflicting_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> conflicting_sink =
        std::make_unique<controlled_native_sink>(conflicting_state);
    auto conflict =
        scope.engine().reserve_backend(fixture.connection.target, conflicting_sink, rumble_mask);
    CHECK(!conflict.has_value());
    if (!conflict.has_value()) {
        CHECK(conflict.error() == execution_error::backend_conflict);
    }
    CHECK(conflicting_sink != nullptr);

    const auto wrong_key_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> wrong_key_sink =
        std::make_unique<controlled_native_sink>(wrong_key_state);
    auto wrong_target = fixture.connection.target;
    ++wrong_target.device.value;
    auto wrong_key =
        scope.engine().reserve_backend(wrong_target, wrong_key_sink, rumble_mask, *first);
    CHECK(!wrong_key.has_value());
    if (!wrong_key.has_value()) {
        CHECK(wrong_key.error() == execution_error::invalid_argument);
    }
    CHECK(wrong_key_sink != nullptr);
}

TEST_CASE("output.execution_replacement_requires_exact_current_and_live_same_key_target",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    const auto first_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> first_sink = std::make_unique<controlled_native_sink>(first_state);
    auto first = scope.register_backend(fixture.connection.target, first_sink, rumble_mask);
    REQUIRE(first.has_value());

    const auto same_connection_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> same_connection_sink =
        std::make_unique<controlled_native_sink>(same_connection_state);
    auto same_connection = scope.register_backend(fixture.connection.target,
        same_connection_sink,
        rumble_mask,
        *first);
    REQUIRE(same_connection.has_value());
    CHECK(same_connection->target == first->target);
    CHECK(same_connection->incarnation.id != first->incarnation.id);
    auto first_progress = scope.engine().progress(*first);
    REQUIRE(first_progress.has_value());
    CHECK(first_progress->phase == backend_phase::retiring);

    std::unique_ptr<source_lifetime> replacement_source =
        std::make_unique<inputnext::test::output::registry_source>();
    auto replacement = fixture.registry.replace(fixture.connection.target, replacement_source);
    REQUIRE(replacement.has_value());
    CHECK(replacement->target.device == fixture.connection.target.device);
    CHECK(replacement->target.connection != fixture.connection.target.connection);

    const auto replacement_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> replacement_sink =
        std::make_unique<controlled_native_sink>(replacement_state);
    auto first_race = scope.engine().reserve_backend(replacement->target,
        replacement_sink,
        rumble_mask,
        *same_connection);
    REQUIRE(first_race.has_value());
    const auto losing_state = std::make_shared<controlled_native_state>();
    std::unique_ptr<native_sink> losing_sink =
        std::make_unique<controlled_native_sink>(losing_state);
    auto losing_race = scope.engine().reserve_backend(replacement->target,
        losing_sink,
        rumble_mask,
        *same_connection);
    REQUIRE(losing_race.has_value());
    auto winning = scope.engine().commit_backend(*first_race, replacement_sink);
    REQUIRE(winning.has_value());
    auto losing = scope.engine().commit_backend(*losing_race, losing_sink);
    CHECK(!losing.has_value());
    if (!losing.has_value()) {
        CHECK(losing.error() == execution_error::stale_backend);
    }
    CHECK(losing_sink != nullptr);
    CHECK(scope.engine().cancel_backend(*losing_race).has_value());
    CHECK(winning->target == replacement->target);
    CHECK(same_connection->target == fixture.connection.target);

    auto old_receipt = scope.engine().retire(*same_connection);
    REQUIRE(old_receipt.has_value());
    auto first_receipt = scope.engine().retire(*first);
    REQUIRE(first_receipt.has_value());
    auto lane = scope.engine().lane_for(*winning);
    REQUIRE(lane.has_value());
    CHECK(scope.engine().drive_one(*lane));
    CHECK(scope.engine().drive_one(*lane));
    auto old_progress = scope.engine().progress(*same_connection);
    auto first_old_progress = scope.engine().progress(*first);
    REQUIRE(old_progress.has_value());
    REQUIRE(first_old_progress.has_value());
    CHECK(old_progress->phase == backend_phase::drained);
    CHECK(first_old_progress->phase == backend_phase::drained);
    CHECK(scope.engine().acknowledge_retirement(*old_receipt).has_value());
    CHECK(scope.engine().acknowledge_retirement(*first_receipt).has_value());
    auto winning_progress = scope.engine().progress(*winning);
    REQUIRE(winning_progress.has_value());
    CHECK(winning_progress->phase == backend_phase::current);
}

TEST_CASE("output.execution_worker_start_failure_releases_unstarted_reservation",
    "[component][output][execution]") {
    authority_fixture fixture{};
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());

    auto first = scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(first.has_value());
    REQUIRE(first->start_worker);
    scope.remember(first->lane);
    const auto failed_start = scope.worker_start_failed(first->lane);
    CHECK(failed_start.has_value());
    CHECK(candidate != nullptr);
    const auto canceled_reservation = scope.engine().cancel_backend(*first);
    CHECK(!canceled_reservation.has_value());

    auto second = scope.engine().reserve_backend(fixture.connection.target, candidate, rumble_mask);
    REQUIRE(second.has_value());
    REQUIRE(second->start_worker);
    CHECK(second->lane.slot == first->lane.slot);
    CHECK(second->lane.generation != first->lane.generation);
    scope.remember(second->lane);
    CHECK(scope.worker_start_failed(second->lane).has_value());
    CHECK(candidate != nullptr);
    CHECK(!scope.engine().has_current_backends());
}

} // namespace
