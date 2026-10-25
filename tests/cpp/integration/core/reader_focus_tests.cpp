#include "core_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_history.hpp"
#include "service_test_access.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <latch>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("core.foreground_reader_rechecks_freshness_after_waiting_for_commit_gate",
    "[integration][core][reader][focus]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::foreground_process);
    REQUIRE(opened.has_value());
    REQUIRE(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(23U),
            .source_timestamp_us = 190U,
            .observation_qpc = 1'900U}});
    auto operation_result = inputnext::core::detail::service_test_access::begin_read(*fixture.core,
        *opened,
        inputnext::history_batch_capacity);
    REQUIRE(operation_result.has_value());
    auto& operation = *operation_result;
    REQUIRE(inputnext::core::detail::service_test_access::qualify_read(*fixture.core, operation) ==
            inputnext::core::status::ok);
    auto batch_result =
        inputnext::core::detail::service_test_access::obtain_batch(*fixture.core, operation);
    REQUIRE(batch_result.has_value());
    const auto& batch = *batch_result;

    auto reader_gate =
        inputnext::core::detail::service_test_access::lock_reader_gate(*fixture.core);
    std::latch foreground_guard_acquired{1};
    inputnext::core::reader_result committed{};
    std::jthread commit_thread([&] {
        const auto guard = fixture.focus_observer->lock_presentation();
        foreground_guard_acquired.count_down();
        committed = inputnext::core::detail::service_test_access::commit_read_state(*fixture.core,
            operation,
            batch,
            &guard);
    });
    foreground_guard_acquired.wait();
    fixture.clock.advance(251ms);
    reader_gate.unlock();
    commit_thread.join();

    CHECK(committed.outcome == inputnext::core::status::not_admitted);
    REQUIRE(fixture.wait_for_focus(inputnext::focus::evidence::foreground));
    const auto replay = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    REQUIRE(replay.outcome == inputnext::core::status::ok);
    REQUIRE(replay.batch.count == 1U);
    CHECK(replay.batch.records[0U].sample.state.buttons == 23U);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
}

TEST_CASE("core.foreground_reader_open_rechecks_freshness_after_waiting_for_commit_gate",
    "[integration][core][reader][focus]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    auto operation_result =
        inputnext::core::detail::service_test_access::prepare_open(*fixture.core,
            target,
            inputnext::core::reader_focus::foreground_process);
    REQUIRE(operation_result.has_value());
    auto operation = *operation_result;
    REQUIRE(inputnext::core::detail::service_test_access::open_native(*fixture.core, operation)
            .has_value());

    auto reader_gate =
        inputnext::core::detail::service_test_access::lock_reader_gate(*fixture.core);
    std::latch foreground_guard_acquired{1};
    inputnext::core::status committed{};
    std::jthread commit_thread([&] {
        const auto guard = fixture.focus_observer->lock_presentation();
        foreground_guard_acquired.count_down();
        committed = inputnext::core::detail::service_test_access::commit_open(*fixture.core,
            operation,
            &guard);
    });
    foreground_guard_acquired.wait();
    fixture.clock.advance(251ms);
    reader_gate.unlock();
    commit_thread.join();

    CHECK(committed == inputnext::core::status::not_admitted);
    CHECK(fixture.core->close_reader(
              inputnext::core::reader_handle{.runtime = fixture.core->info().runtime,
                  .id = operation.native.value}) == inputnext::core::status::ok);
    REQUIRE(fixture.wait_for_focus(inputnext::focus::evidence::foreground));

    const auto replacement =
        fixture.core->open_reader(target, inputnext::core::reader_focus::foreground_process);
    REQUIRE(replacement.has_value());
    CHECK(fixture.core->read_events(*replacement, 1U).outcome == inputnext::core::status::ok);
    CHECK(fixture.core->close_reader(*replacement) == inputnext::core::status::ok);
}

TEST_CASE("core.foreground_reader_preserves_pending_history_across_focus_loss",
    "[integration][core][reader][focus]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::foreground_process);
    REQUIRE(opened.has_value());
    REQUIRE(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->enqueue(inputnext::gameinput_detail::native_history_step{
        .kind = inputnext::gameinput_detail::native_history_step_kind::reading,
        .reading = {.state = inputnext::gameinput_test_support::test_pad(11U),
            .source_timestamp_us = 150U,
            .observation_qpc = 1'500U}});
    fixture.scripts[0U]->block_next();
    inputnext::core::reader_result suppressed{};
    std::jthread reader([&] {
        suppressed = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    });
    inputnext::core_test_support::history_call_release_guard release{fixture.scripts[0U]};
    REQUIRE(fixture.scripts[0U]->wait_until_blocked(2s));

    fixture.set_focus(inputnext::focus::evidence::background);
    REQUIRE(fixture.wait_for_focus(inputnext::focus::evidence::background));
    release.release();
    reader.join();
    CHECK(suppressed.outcome == inputnext::core::status::not_admitted);
    CHECK(suppressed.batch.count == 0U);

    fixture.set_focus(inputnext::focus::evidence::foreground);
    REQUIRE(fixture.wait_for_focus(inputnext::focus::evidence::foreground));
    const auto replay = fixture.core->read_events(*opened, inputnext::history_batch_capacity);
    REQUIRE(replay.outcome == inputnext::core::status::ok);
    REQUIRE(replay.batch.count == 1U);
    CHECK(replay.batch.records[0U].sample.state.buttons == 11U);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
}
