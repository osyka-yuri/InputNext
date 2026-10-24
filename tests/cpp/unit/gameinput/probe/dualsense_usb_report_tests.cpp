#include "dualsense_usb_report.hpp"

#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace inputnext::gameinput_detail {

TEST_CASE("gameinput.probe.usb_delivery_report_preserves_unconfirmed_status",
    "[gameinput][probe][unit][report]") {
    CHECK(format_dualsense_usb_progress(std::nullopt) ==
          "dualsense_usb_delivery backend=unavailable native_status=unconfirmed");
    output::backend_progress progress{};
    progress.backend.incarnation.id = output::backend_id{7U};
    progress.observed_publication = output::publication_id{8U};
    progress.write_in_flight = true;
    progress.consecutive_retryable_failures = 3U;
    progress.scheduled_retry_delay = std::chrono::milliseconds{64};
    const auto report = format_dualsense_usb_progress(progress);
    CHECK(report.contains("backend=7 publication=8 in_flight=true"));
    CHECK(report.contains("retryable_failures=3 retry_delay_ms=64"));
    CHECK(report.contains("native_status=not_completed"));
}

TEST_CASE("gameinput.probe.usb_delivery_report_identifies_completed_native_failure",
    "[gameinput][probe][unit][report]") {
    output::backend_progress progress{};
    progress.last_completed_attempt = output::completed_attempt{.id = output::attempt_id{9U},
        .publication = output::publication_id{4U},
        .result = {.status = output::native_status::retryable_failure, .native_code = 123U}};
    const auto report = format_dualsense_usb_progress(progress);
    CHECK(report.contains("completed_attempt=9 completed_publication=4"));
    CHECK(report.contains("native_status=retryable_failure native_code=123"));
}

} // namespace inputnext::gameinput_detail
