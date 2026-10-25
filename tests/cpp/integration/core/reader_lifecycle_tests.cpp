#include "core_test_support.hpp"

#include "core/core.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("core.ready_reader_rejects_foreign_runtime_handles_without_invalidating_its_reader",
    "[integration][core][reader][identity]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());

    auto foreign = *opened;
    foreign.runtime.bytes[0U] ^= std::byte{0x80};
    CHECK(fixture.core->read_events(foreign, inputnext::history_batch_capacity).outcome ==
          inputnext::core::status::stale_reader);
    CHECK(fixture.core->close_reader(foreign) == inputnext::core::status::stale_reader);

    const auto original = fixture.core->read_events(*opened, 1U);
    CHECK(original.outcome == inputnext::core::status::ok);
    CHECK(original.batch.count == 1U);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
}

TEST_CASE("core.reader_capacity_is_limited_to_the_public_reader_pool",
    "[integration][core][reader][capacity]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    std::vector<inputnext::core::reader_handle> readers;
    readers.reserve(inputnext::core::reader_capacity);

    for (std::size_t index = 0U; index < inputnext::core::reader_capacity; ++index) {
        const auto opened =
            fixture.core->open_reader(target, inputnext::core::reader_focus::background);
        REQUIRE(opened.has_value());
        readers.push_back(*opened);
    }

    const auto excess =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE_FALSE(excess.has_value());
    CHECK(excess.error() == inputnext::core::status::capacity_exceeded);

    for (const auto reader : readers) {
        CHECK(fixture.core->close_reader(reader) == inputnext::core::status::ok);
    }
}

TEST_CASE("core.reader_close_cancels_a_blocked_read_without_waiting_for_it",
    "[integration][core][reader][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());
    REQUIRE(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->block_next();
    inputnext::core::reader_result read{};
    std::jthread reader(
        [&] { read = fixture.core->read_events(*opened, inputnext::history_batch_capacity); });
    inputnext::core_test_support::history_call_release_guard release{fixture.scripts[0U]};
    REQUIRE(fixture.scripts[0U]->wait_until_blocked(2s));

    CHECK(fixture.core->read_events(*opened, inputnext::history_batch_capacity).outcome ==
          inputnext::core::status::busy);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
    release.release();
    reader.join();
    CHECK(read.outcome == inputnext::core::status::stale_reader);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::stale_reader);
}

TEST_CASE("core.readers_on_different_devices_progress_independently",
    "[integration][core][reader][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    const auto first_target = fixture.connect(0U);
    const auto second_target = fixture.connect(1U);
    const auto first =
        fixture.core->open_reader(first_target, inputnext::core::reader_focus::background);
    const auto second =
        fixture.core->open_reader(second_target, inputnext::core::reader_focus::background);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(fixture.core->read_events(*first, 1U).outcome == inputnext::core::status::ok);
    REQUIRE(fixture.core->read_events(*second, 1U).outcome == inputnext::core::status::ok);

    fixture.scripts[0U]->block_next();
    inputnext::core::reader_result blocked_result{};
    std::jthread blocked_read([&] {
        blocked_result = fixture.core->read_events(*first, inputnext::history_batch_capacity);
    });
    inputnext::core_test_support::history_call_release_guard release{fixture.scripts[0U]};
    REQUIRE(fixture.scripts[0U]->wait_until_blocked(2s));

    CHECK(fixture.core->read_events(*second, inputnext::history_batch_capacity).outcome ==
          inputnext::core::status::no_data);
    release.release();
    blocked_read.join();
    CHECK(blocked_result.outcome == inputnext::core::status::no_data);
    CHECK(fixture.core->close_reader(*first) == inputnext::core::status::ok);
    CHECK(fixture.core->close_reader(*second) == inputnext::core::status::ok);
}

TEST_CASE("core.close_accounts_for_readers_already_drained_by_runtime_shutdown",
    "[integration][core][reader][shutdown]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto opened =
        fixture.core->open_reader(target, inputnext::core::reader_focus::background);
    REQUIRE(opened.has_value());

    fixture.core->close_admission();
    CHECK(fixture.core->read_events(*opened, 1U).outcome == inputnext::core::status::unavailable);
    CHECK(fixture.gameinput.runtime->stop(inputnext::maintenance_time::clock::now()).drained);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::ok);
    CHECK(fixture.core->close_reader(*opened) == inputnext::core::status::stale_reader);
    CHECK(fixture.core->close_reader(
              inputnext::core::reader_handle{.runtime = fixture.core->info().runtime,
                  .id = opened->id + 1000U}) == inputnext::core::status::stale_reader);
}
