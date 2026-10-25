#include "core_test_support.hpp"

#include "core/core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_CASE("core.latest_reads_the_coherent_registry_sample_without_metadata_queries",
    "[integration][core][latest]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    const auto metadata_queries = fixture.gameinput.metadata->query_calls();
    const auto native_current_calls = fixture.gameinput.native->current_calls;

    const auto snapshot = fixture.core->read_latest(target);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->target == target);
    CHECK(snapshot->sample.state.buttons == 3U);
    CHECK(snapshot->sample.source_timestamp_us == 120U);
    CHECK(snapshot->sample.observation_qpc == 1'200U);
    CHECK(snapshot->sample.revision != 0U);
    CHECK(fixture.gameinput.metadata->query_calls() == metadata_queries);
    CHECK(fixture.gameinput.native->current_calls == native_current_calls);
}

TEST_CASE("core.latest_reports_no_data_for_a_live_connection_without_a_sample",
    "[integration][core][latest]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect(0U, false);

    const auto snapshot = fixture.core->read_latest(target);
    REQUIRE_FALSE(snapshot.has_value());
    CHECK(snapshot.error() == inputnext::core::status::no_data);
}

TEST_CASE("core.latest_rejects_foreign_and_retired_targets", "[integration][core][latest]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();

    auto foreign = target;
    foreign.runtime.bytes[0U] = std::byte{0x99};
    CHECK(fixture.core->read_latest(foreign).error() == inputnext::core::status::stale_target);

    REQUIRE(fixture.gameinput.registry.retire(target).has_value());
    CHECK(fixture.core->read_latest(target).error() == inputnext::core::status::stale_target);
}
