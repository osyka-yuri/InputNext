#include "core_test_support.hpp"

#include "core/core.hpp"
#include "service_test_access.hpp"
#include <inputnext/history.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <latch>
#include <memory>
#include <thread>

namespace {

struct replacement_source final : inputnext::source_lifetime {};

} // namespace

TEST_CASE("core.reader_open_rechecks_target_after_waiting_for_commit",
    "[integration][core][reader][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    auto prepared = inputnext::core::detail::service_test_access::prepare_open(*fixture.core,
        target,
        inputnext::core::reader_focus::background);
    REQUIRE(prepared.has_value());
    auto& operation = *prepared;
    REQUIRE(inputnext::core::detail::service_test_access::open_native(*fixture.core, operation)
            .has_value());
    const bool replace_connection = GENERATE(false, true);
    INFO("Replace connection: " << replace_connection);
    std::unique_ptr<inputnext::source_lifetime> source;
    if (replace_connection) {
        source = std::make_unique<replacement_source>();
    }

    const auto pin = fixture.gameinput.registry.capture(target);
    REQUIRE(pin.has_value());
    std::latch commit_started{1};
    inputnext::core::status committed{};
    std::jthread commit_thread;
    auto reader_gate =
        inputnext::core::detail::service_test_access::lock_reader_gate(*fixture.core);
    commit_thread = std::jthread([&] {
        commit_started.count_down();
        committed = inputnext::core::detail::service_test_access::commit_open(*fixture.core,
            operation,
            nullptr);
    });
    commit_started.wait();
    bool mutation_succeeded{};
    inputnext::connection_target replacement{};
    if (replace_connection) {
        const auto replaced = fixture.gameinput.registry.replace(target, source);
        mutation_succeeded = replaced.has_value();
        if (replaced.has_value()) {
            replacement = replaced->target;
        }
    } else {
        mutation_succeeded = fixture.gameinput.registry.retire(target).has_value();
    }
    reader_gate.unlock();
    commit_thread.join();

    REQUIRE(mutation_succeeded);
    CHECK(committed == inputnext::core::status::stale_target);
    CHECK(fixture.core->close_reader(
              inputnext::core::reader_handle{.runtime = fixture.core->info().runtime,
                  .id = operation.native.value}) == inputnext::core::status::ok);
    if (replace_connection) {
        CHECK(fixture.gameinput.registry.capture(replacement).has_value());
        CHECK(replacement.device == target.device);
        CHECK(replacement.connection != target.connection);
        CHECK(fixture.gameinput.registry.retire(replacement).has_value());
    }
}

TEST_CASE("core.reader_open_can_succeed_before_a_later_retirement",
    "[integration][core][reader][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto reader =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(reader.has_value());
    REQUIRE(fixture.gameinput.registry.retire(target).has_value());
    CHECK(fixture.core->read_events(*reader, inputnext::history_batch_capacity).outcome ==
          inputnext::core::status::source_retired);
    CHECK(fixture.core->close_reader(*reader) == inputnext::core::status::ok);
}

TEST_CASE("core.reader_open_rejects_admission_closed_while_waiting_for_commit",
    "[integration][core][reader][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    auto prepared = inputnext::core::detail::service_test_access::prepare_open(*fixture.core,
        target,
        inputnext::core::reader_focus::background);
    REQUIRE(prepared.has_value());
    auto& operation = *prepared;
    REQUIRE(inputnext::core::detail::service_test_access::open_native(*fixture.core, operation)
            .has_value());

    std::latch commit_started{1};
    inputnext::core::status committed{};
    std::jthread commit_thread;
    auto reader_gate =
        inputnext::core::detail::service_test_access::lock_reader_gate(*fixture.core);
    commit_thread = std::jthread([&] {
        commit_started.count_down();
        committed = inputnext::core::detail::service_test_access::commit_open(*fixture.core,
            operation,
            nullptr);
    });
    commit_started.wait();
    fixture.core->close_admission();
    reader_gate.unlock();
    commit_thread.join();

    CHECK(committed == inputnext::core::status::unavailable);
    CHECK(fixture.core->close_reader(
              inputnext::core::reader_handle{.runtime = fixture.core->info().runtime,
                  .id = operation.native.value}) == inputnext::core::status::ok);
}
