#include "gameinput_probe/input/history/evidence.hpp"

#include "gameinput_probe/input/history/report.hpp"
#include "gameinput_probe/report.hpp"
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

TEST_CASE("gameinput.probe.native_history_requires_baseline_and_progress",
    "[unit][gameinput][probe]") {
    history_evidence evidence{};
    history_batch baseline{};
    baseline.source = history_source_kind::native_history;
    baseline.count = 1U;
    baseline.records[0U] = {.sequence = 1U, .baseline = true};
    evidence.observe(baseline);
    CHECK(evidence.baselines == 1U);
    CHECK(evidence.native_readings == 0U);
    CHECK_FALSE(evidence.native_history_observed());

    history_batch unqualified{};
    unqualified.source = history_source_kind::native_history;
    unqualified.caught_up = true;
    evidence.observe(unqualified);
    CHECK_FALSE(evidence.native_history_observed());

    history_batch checkpoint{};
    checkpoint.source = history_source_kind::native_history;
    checkpoint.caught_up = true;
    checkpoint.checkpoint_qpc = 900U;
    evidence.observe(checkpoint);
    CHECK(evidence.native_history_observed());
    CHECK(evidence.native_readings == 0U);
}

TEST_CASE("gameinput.probe.callback_fallback_is_distinct_from_native_history",
    "[unit][gameinput][probe]") {
    history_evidence evidence{};
    history_batch captured{};
    captured.count = 2U;
    captured.records[0U] = {.sequence = 1U, .baseline = true};
    captured.records[1U] = {
        .sequence = 2U,
        .origin = history_record_origin::callback_pad,
    };
    captured.caught_up = true;
    captured.checkpoint_qpc = 1000U;
    evidence.observe(captured);
    CHECK(evidence.readings == 1U);
    CHECK(evidence.native_readings == 0U);
    CHECK_FALSE(evidence.native_history_observed());
    CHECK_FALSE(format_requirements({.history_required = true}).met);
    CHECK(format_requirements({.history_required = true, .history_observed = true}).met);
}

TEST_CASE("gameinput.probe.history_report_keeps_prefix_and_failure", "[unit][gameinput][probe]") {
    history_batch batch{};
    batch.source = history_source_kind::native_history;
    batch.count = 1U;
    batch.records[0U] = {
        .sample = {.source_timestamp_us = 51U, .effective_qpc = 600U, .guide_known = false},
        .sequence = 2U,
        .origin = history_record_origin::callback_guide,
    };
    batch.first_sequence = 2U;
    batch.last_sequence = 2U;
    batch.domain.target.device.value = 3U;
    batch.domain.target.connection.value = 7U;
    batch.domain.stream = 4U;
    batch.failure =
        history_failure{.code = history_error::provider_failure, .native_code = 0xDEADU};
    history_evidence evidence{};
    evidence.observe(batch);

    const auto report = format_history_batch(batch.domain.target, batch, evidence);
    CHECK(evidence.readings == 1U);
    CHECK(evidence.failures == 1U);
    CHECK(report.contains("connection=7 stream=4 source=native_history records=1 first=2 last=2"));
    CHECK(report.contains("error=provider_failure native_code=0xdead"));
    CHECK(format_history_record(batch.domain, batch.records[0U])
            .contains(
                "sequence=2 baseline=false source_us=51 qpc=600 buttons=0x0 guide_known=false "
                "origin=callback_guide"));
}

TEST_CASE("gameinput.probe.stop_report_distinguishes_history_calls_and_owners",
    "[unit][gameinput][probe]") {
    const auto report = format_stop_report({.drained = false,
        .pending_callback_owners = 1U,
        .active_metadata_queries = 2U,
        .active_history_calls = 3U,
        .pending_history_owners = 4U});
    CHECK(report ==
          "stop=pending callbacks_pending=1 metadata_queries=2 history_calls=3 history_owners=4");
}

} // namespace
