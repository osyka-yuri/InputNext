#include "input_qualification/report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <ios>
#include <span>
#include <sstream>
#include <string>

using inputnext::input_qualification::bootstrap_attempt_capacity_available;
using inputnext::input_qualification::bootstrap_attempt_summary;
using inputnext::input_qualification::bootstrap_measurement_summary;
using inputnext::input_qualification::classify_capture_qualification;
using inputnext::input_qualification::dualsense_usb_touch_payload_equal;
using inputnext::input_qualification::dualsense_usb_touch_record;
using inputnext::input_qualification::escape_json_string;
using inputnext::input_qualification::has_any_touch_contacts;
using inputnext::input_qualification::native_capture_summary;
using inputnext::input_qualification::qualification_capture_evidence;
using inputnext::input_qualification::reading_fields_complete;
using inputnext::input_qualification::reading_payload_equal;
using inputnext::input_qualification::reading_sample;
using inputnext::input_qualification::saw_unlabeled_button_transition;
using inputnext::input_qualification::write_bootstrap_measurement_json;
using inputnext::input_qualification::write_dualsense_usb_touch_json;
using inputnext::input_qualification::write_hex_json;
using inputnext::input_qualification::write_native_capture_prefix;

namespace {

qualification_capture_evidence complete_qualification_evidence() {
    return qualification_capture_evidence{.raw_usb_requested = false,
        .raw_usb_enabled = false,
        .has_touch_metadata = true,
        .button_labels_complete = true,
        .descriptor_fields_complete = true,
        .native_forward_complete = true,
        .native_forward_truncated = false,
        .callback_unlabeled_activity = false,
        .native_unlabeled_activity = false,
        .callback_records = 10U,
        .callback_drops = 0U,
        .callback_incomplete_records = 0U,
        .callback_identity_failures = 0U,
        .callback_timestamp_order_violations = 0U,
        .native_records = 10U,
        .native_incomplete_records = 0U,
        .native_identity_failures = 0U,
        .native_timestamp_order_violations = 0U,
        .identity_pairs = 4U,
        .payload_mismatches = 0U,
        .gesture_payload_matches = 1U,
        .callback_raw_reads_available = 0U,
        .callback_raw_reads_missing = 0U,
        .native_raw_reads_available = 0U,
        .native_raw_reads_missing = 0U,
        .raw_identity_pairs = 0U,
        .raw_payload_mismatches = 0U};
}

} // namespace

TEST_CASE("hexadecimal JSON scalars preserve quotes and stream formatting") {
    std::ostringstream json;
    json << R"({"identity":)";
    write_hex_json(json, 0U);
    json << R"(,"button_bits_hex":)";
    write_hex_json(json, ~std::uint64_t{0U});
    json << R"(,"decimal":)" << 123U << '}';

    REQUIRE(
        json.str() == R"({"identity":"0x0","button_bits_hex":"0xffffffffffffffff","decimal":123})");

    std::ostringstream formatted;
    formatted << std::hex << std::uppercase;
    write_hex_json(formatted, 10U);
    formatted << ':' << 255U;
    REQUIRE(formatted.str() == R"("0xa":FF)");
}

TEST_CASE("JSON strings preserve UTF-8 and escape quotes backslashes and controls") {
    const std::string input = "Путь \"A\\B\"\n\t";

    REQUIRE(escape_json_string(input) == "Путь \\\"A\\\\B\\\"\\u000a\\u0009");
}

TEST_CASE("raw DualSense touch observations serialize availability and exact contacts") {
    const dualsense_usb_touch_record pressed{.attempted = true,
        .click_state_available = true,
        .click_pressed = true,
        .available = true,
        .side_evaluated = true,
        .resolved_side = inputnext::input_qualification::dualsense_usb_touch_side::left,
        .contact_count = 2U,
        .contacts = {
            inputnext::input_qualification::dualsense_usb_touch_point{.id = 3U, .x = 959, .y = 540},
            inputnext::input_qualification::dualsense_usb_touch_point{.id = 4U,
                .x = 960,
                .y = 541}}};
    std::ostringstream json;
    write_dualsense_usb_touch_json(json, pressed);
    REQUIRE(
        json.str() ==
        R"({"attempted":true,"click_state_available":true,"click_pressed":true,"available":true,"resolved_side":"left","contact_count":2,"contacts":[{"id":3,"x":959,"y":540},{"id":4,"x":960,"y":541}]})");
    REQUIRE(dualsense_usb_touch_payload_equal(pressed, pressed));
    auto changed_contact = pressed;
    changed_contact.contacts[1].x = 961;
    REQUIRE_FALSE(dualsense_usb_touch_payload_equal(pressed, changed_contact));
    auto changed_side = pressed;
    changed_side.resolved_side = inputnext::input_qualification::dualsense_usb_touch_side::right;
    REQUIRE(dualsense_usb_touch_payload_equal(pressed, changed_side));

    const dualsense_usb_touch_record no_contacts{.attempted = true,
        .click_state_available = true,
        .click_pressed = false,
        .available = true,
        .side_evaluated = true,
        .resolved_side = inputnext::input_qualification::dualsense_usb_touch_side::none,
        .contact_count = 0U,
        .contacts = {}};
    std::ostringstream empty_json;
    write_dualsense_usb_touch_json(empty_json, no_contacts);
    REQUIRE(
        empty_json.str() ==
        R"({"attempted":true,"click_state_available":true,"click_pressed":false,"available":true,"resolved_side":"none","contact_count":0,"contacts":[]})");
    REQUIRE_FALSE(dualsense_usb_touch_payload_equal(pressed, no_contacts));

    const dualsense_usb_touch_record unavailable{.attempted = true,
        .click_state_available = false,
        .click_pressed = false,
        .available = false,
        .side_evaluated = false,
        .resolved_side = inputnext::input_qualification::dualsense_usb_touch_side::none,
        .contact_count = 0U,
        .contacts = {}};
    std::ostringstream unavailable_json;
    write_dualsense_usb_touch_json(unavailable_json, unavailable);
    REQUIRE(
        unavailable_json.str() ==
        R"({"attempted":true,"click_state_available":false,"click_pressed":false,"available":false,"resolved_side":"not_evaluated","contact_count":0,"contacts":[]})");
    REQUIRE(dualsense_usb_touch_payload_equal(unavailable, unavailable));
}

TEST_CASE("native capture summary serializes as a well-formed JSON object") {
    const native_capture_summary summary{.status = "forward_capture_stream",
        .filter_input_kind = 4456452U,
        .records = 2U,
        .record_capacity = 8192U,
        .truncated = false,
        .identity_failures = 0U,
        .transition_count = 1U,
        .stop_reason = "previous_unavailable",
        .seed_hresult = 0,
        .last_hresult = 0,
        .no_next_polls = 3U,
        .next_stop_reason = "not_applicable_forward_walk",
        .next_transitions = 0U,
        .next_identity_matches = 0U,
        .callback_native_identity_matches = 1U,
        .native_records_seen_in_callback = 1U,
        .payload_matches = 1U,
        .payload_mismatches = 0U,
        .gesture_bearing_identity_matches = 1U,
        .gesture_bearing_payload_matches = 1U,
        .same_timestamp_distinct_readings = 1U,
        .timestamp_order_violations = 0U};
    std::ostringstream json;
    json << "{\"native_capture\":";
    write_native_capture_prefix(json, summary);
    json << "]}}";

    REQUIRE(
        json.str() ==
        R"({"native_capture":{"status":"forward_capture_stream","filter_input_kind":4456452,"records":2,"record_capacity":8192,"truncated":false,"identity_failures":0,"transition_count":1,"stop_reason":"previous_unavailable","seed_hresult":0,"last_hresult":0,"no_next_polls":3,"next_stop_reason":"not_applicable_forward_walk","next_transitions":0,"next_identity_matches":0,"same_timestamp_distinct_readings":1,"timestamp_order_violations":0,"callback_native_identity_matches":1,"native_records_seen_in_callback":1,"payload_matches":1,"payload_mismatches":0,"gesture_bearing_identity_matches":1,"gesture_bearing_payload_matches":1,"records_detail":[]}})");
}

TEST_CASE("unlabeled physical button transitions remain observable without semantic mapping") {
    SECTION("a None label and a missing label are treated as unlabeled") {
        const std::array<std::int32_t, 4U> labels{5, 6, 0, 0};
        const std::array<std::uint64_t, 2U> state_masks{0U, std::uint64_t{1U} << 2U};

        REQUIRE(saw_unlabeled_button_transition(4U, labels, state_masks));
    }

    SECTION("known semantic buttons do not count as unlabeled activity") {
        const std::array<std::int32_t, 2U> labels{5, 6};
        const std::array<std::uint64_t, 2U> state_masks{0U, 1U};

        REQUIRE_FALSE(saw_unlabeled_button_transition(2U, labels, state_masks));
    }

    SECTION("no state transition produces no activity") {
        const std::array<std::int32_t, 1U> labels{0};
        const std::array<std::uint64_t, 3U> state_masks{0U, 0U, 0U};

        REQUIRE_FALSE(saw_unlabeled_button_transition(1U, labels, state_masks));
    }
}

TEST_CASE("qualification applies one integrity gate to all observation routes") {
    auto no_metadata = complete_qualification_evidence();
    no_metadata.has_touch_metadata = false;
    no_metadata.gesture_payload_matches = 0U;

    auto raw = complete_qualification_evidence();
    raw.raw_usb_requested = true;
    raw.raw_usb_enabled = true;
    raw.callback_raw_reads_available = 1U;
    raw.native_raw_reads_available = 1U;
    raw.raw_identity_pairs = 1U;

    const std::array<qualification_capture_evidence, 3U> routes{no_metadata,
        raw,
        complete_qualification_evidence()};
    for (const auto& route : routes) {
        auto incomplete_callback = route;
        incomplete_callback.callback_drops = 1U;
        REQUIRE(classify_capture_qualification(incomplete_callback) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto empty_callback = route;
        empty_callback.callback_records = 0U;
        REQUIRE(classify_capture_qualification(empty_callback) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto incomplete_callback_fields = route;
        incomplete_callback_fields.callback_incomplete_records = 1U;
        REQUIRE(classify_capture_qualification(incomplete_callback_fields) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto incomplete_native = route;
        incomplete_native.native_forward_complete = false;
        REQUIRE(classify_capture_qualification(incomplete_native) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto empty_native = route;
        empty_native.native_records = 0U;
        REQUIRE(classify_capture_qualification(empty_native) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto truncated_native = route;
        truncated_native.native_forward_truncated = true;
        REQUIRE(classify_capture_qualification(truncated_native) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto mismatched_payload = route;
        mismatched_payload.payload_mismatches = 1U;
        REQUIRE(classify_capture_qualification(mismatched_payload) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto no_identity_overlap = route;
        no_identity_overlap.identity_pairs = 0U;
        REQUIRE(classify_capture_qualification(no_identity_overlap) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto callback_order_regression = route;
        callback_order_regression.callback_timestamp_order_violations = 1U;
        REQUIRE(classify_capture_qualification(callback_order_regression) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));

        auto native_order_regression = route;
        native_order_regression.native_timestamp_order_violations = 1U;
        REQUIRE(classify_capture_qualification(native_order_regression) ==
                (route.raw_usb_requested
                        ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                        : "inconclusive_forward_native_coverage_or_gesture_overlap"));
    }
}

TEST_CASE("raw idle readings remain useful and typed gesture claims need matched payload") {
    auto raw_idle = complete_qualification_evidence();
    raw_idle.raw_usb_requested = true;
    raw_idle.raw_usb_enabled = true;
    raw_idle.callback_raw_reads_available = 1U;
    raw_idle.native_raw_reads_available = 1U;
    raw_idle.raw_identity_pairs = 1U;
    raw_idle.gesture_payload_matches = 0U;
    REQUIRE(classify_capture_qualification(raw_idle) ==
            "raw_usb_touch_observations_captured_manual_review_required_no_auto_qualification");

    auto typed_without_gesture = complete_qualification_evidence();
    typed_without_gesture.gesture_payload_matches = 0U;
    REQUIRE(classify_capture_qualification(typed_without_gesture) ==
            "inconclusive_forward_native_coverage_or_gesture_overlap");
    typed_without_gesture.gesture_payload_matches = 1U;
    REQUIRE(classify_capture_qualification(typed_without_gesture) ==
            "observations_captured_manual_review_required_no_auto_qualification");
}

TEST_CASE("complete no-metadata captures report the absence conservatively") {
    auto evidence = complete_qualification_evidence();
    evidence.has_touch_metadata = false;
    evidence.gesture_payload_matches = 0U;
    REQUIRE(classify_capture_qualification(evidence) ==
            "touch_metadata_not_exposed_no_unlabeled_change_observed_manual_review_required");

    evidence.button_labels_complete = false;
    REQUIRE(classify_capture_qualification(evidence) ==
            "inconclusive_touch_metadata_labels_incomplete");
    evidence.callback_unlabeled_activity = true;
    evidence.gesture_payload_matches = 1U;
    REQUIRE(classify_capture_qualification(evidence) ==
            "inconclusive_touch_metadata_labels_incomplete");

    evidence.button_labels_complete = true;
    evidence.gesture_payload_matches = 0U;
    REQUIRE(classify_capture_qualification(evidence) ==
            "inconclusive_forward_native_coverage_or_gesture_overlap");
    evidence.gesture_payload_matches = 1U;
    REQUIRE(classify_capture_qualification(evidence) ==
            "unlabeled_button_change_observed_manual_review_required");
}

TEST_CASE("gesture contact detection includes every advertised surface") {
    const std::array<std::uint32_t, 3U> contact_counts{0U, 2U, 0U};
    REQUIRE(has_any_touch_contacts(contact_counts));
    REQUIRE_FALSE(has_any_touch_contacts(std::array<std::uint32_t, 3U>{0U, 0U, 0U}));
}

TEST_CASE("reading overlap requires the same payload and exact descriptor counts") {
    SECTION("payload changes on the same captured identity remain detectable") {
        reading_sample callback{};
        callback.timestamp = 42U;
        callback.gamepad_available = true;
        callback.surface_count = 2U;
        callback.returned_contact_counts[1] = 1U;
        callback.contacts[1][0] = {.id = 7U, .x = 120, .y = 240, .pad_x = 120U, .pad_y = 240U};
        auto native = callback;

        REQUIRE(reading_payload_equal(callback, native));
        native.contacts[1][0].x++;
        REQUIRE_FALSE(reading_payload_equal(callback, native));
    }

    SECTION("button and surface counts must match the captured descriptor") {
        auto reading = reading_sample{.gamepad_available = true,
            .reading_button_count = 15U,
            .returned_button_count = 15U,
            .surface_count = 2U,
            .reported_contact_counts = {0U, 1U},
            .returned_contact_counts = {0U, 1U}};

        REQUIRE(reading_fields_complete(reading, 15U, 2U));
        reading.reading_button_count = 16U;
        reading.returned_button_count = 16U;
        REQUIRE_FALSE(reading_fields_complete(reading, 15U, 2U));
        reading.reading_button_count = 15U;
        reading.returned_button_count = 15U;
        reading.surface_count = 1U;
        REQUIRE_FALSE(reading_fields_complete(reading, 15U, 2U));
    }

    SECTION("complete contacts on a second surface are included") {
        const auto reading = reading_sample{.gamepad_available = true,
            .reading_button_count = 15U,
            .returned_button_count = 15U,
            .surface_count = 2U,
            .reported_contact_counts = {0U, 2U},
            .returned_contact_counts = {0U, 2U}};

        REQUIRE(reading_fields_complete(reading, 15U, 2U));
    }
}

TEST_CASE("bootstrap measurement report serializes an empty bounded run") {
    const bootstrap_measurement_summary summary{.requested = true,
        .click_index_valid = true,
        .raw_usb_coordinates = false,
        .source_baseline_observed = false,
        .click_button = 13U,
        .observed_button_count = 15U,
        .qpc_frequency = 10'000'000U,
        .status = "waiting_for_source_baseline",
        .truncated = false,
        .attempts = {}};
    std::ostringstream json;
    write_bootstrap_measurement_json(json, summary);

    REQUIRE(
        json.str() ==
        R"({"requested":true,"mode":"experimental_unqualified_composite_without_coordinates","status":"waiting_for_source_baseline","click_button":13,"observed_button_count":15,"click_index_valid":true,"source_baseline_observed":false,"time_source":"QueryPerformanceCounter","qpc_frequency":10000000,"capacity":1024,"attempt_count":0,"truncated":false,"succeeded":0,"fallback_or_failed":0,"timing_includes":["factory","start","destruction"],"timing_summary":{"factory_us":{"count":0,"p50":null,"p95":null,"p99":null,"max":null},"start_us":{"count":0,"p50":null,"p95":null,"p99":null,"max":null},"destruction_us":{"count":0,"p50":null,"p95":null,"p99":null,"max":null},"total_us":{"count":0,"p50":null,"p95":null,"p99":null,"max":null}},"fallback_by_reason":{},"attempts":[]})");

    auto raw_summary = summary;
    raw_summary.raw_usb_coordinates = true;
    std::ostringstream raw_json;
    write_bootstrap_measurement_json(raw_json, raw_summary);
    REQUIRE(raw_json.str().contains(
        R"("mode":"experimental_unqualified_composite_with_raw_usb_coordinates")"));
}

TEST_CASE("bootstrap measurement report summarizes timing and fallback causes") {
    const std::array<bootstrap_attempt_summary, 3U> attempts{
        bootstrap_attempt_summary{.timing_available = true,
            .factory_measured = true,
            .start_measured = true,
            .destruction_measured = true,
            .total_measured = true,
            .factory_ticks = 1U,
            .start_ticks = 10U,
            .destruction_ticks = 1U,
            .total_ticks = 12U,
            .transitions = 4U,
            .succeeded = true,
            .fallback = false,
            .native_code = 0,
            .reason = "none",
            .touch_source = "left",
            .touch_reason = "none",
            .context_baseline_available = true,
            .context_click_pressed = true,
            .context_awaiting_release = false,
            .context_has_first_timestamp = true,
            .context_epoch = 2U,
            .context_first_timestamp = 100U},
        bootstrap_attempt_summary{.timing_available = true,
            .factory_measured = true,
            .start_measured = true,
            .destruction_measured = true,
            .total_measured = true,
            .factory_ticks = 2U,
            .start_ticks = 20U,
            .destruction_ticks = 1U,
            .total_ticks = 23U,
            .transitions = 256U,
            .succeeded = false,
            .fallback = true,
            .native_code = -1,
            .reason = "bootstrap_budget",
            .touch_source = "none",
            .touch_reason = "none",
            .context_baseline_available = true,
            .context_click_pressed = true,
            .context_awaiting_release = false,
            .context_has_first_timestamp = true,
            .context_epoch = 2U,
            .context_first_timestamp = 100U},
        bootstrap_attempt_summary{.timing_available = true,
            .factory_measured = true,
            .start_measured = true,
            .destruction_measured = true,
            .total_measured = true,
            .factory_ticks = 4U,
            .start_ticks = 30U,
            .destruction_ticks = 1U,
            .total_ticks = 35U,
            .transitions = 7U,
            .succeeded = false,
            .fallback = true,
            .native_code = -2,
            .reason = "history_lost",
            .touch_source = "none",
            .touch_reason = "none",
            .context_baseline_available = true,
            .context_click_pressed = true,
            .context_awaiting_release = false,
            .context_has_first_timestamp = true,
            .context_epoch = 2U,
            .context_first_timestamp = 100U}};
    const bootstrap_measurement_summary summary{.requested = true,
        .click_index_valid = true,
        .raw_usb_coordinates = false,
        .source_baseline_observed = true,
        .click_button = 13U,
        .observed_button_count = 15U,
        .qpc_frequency = 1'000'000U,
        .status = "collecting_production_cursor_measurements",
        .truncated = false,
        .attempts = std::span<const bootstrap_attempt_summary>{attempts}};
    std::ostringstream json;
    write_bootstrap_measurement_json(json, summary);

    const auto serialized = json.str();
    REQUIRE(serialized.contains(R"("start_us":{"count":3,"p50":20,"p95":30,"p99":30,"max":30})"));
    REQUIRE(serialized.contains(R"("fallback_by_reason":{"history_lost":1,"bootstrap_budget":1})"));
    REQUIRE(
        serialized.contains(R"("bootstrap_transitions":256,"succeeded":false,"fallback":true)"));
}

TEST_CASE("bootstrap measurement accepts the first attempt and stops at its bound") {
    REQUIRE(bootstrap_attempt_capacity_available(0U));
    REQUIRE(bootstrap_attempt_capacity_available(1U));
    REQUIRE(bootstrap_attempt_capacity_available(1023U));
    REQUIRE_FALSE(bootstrap_attempt_capacity_available(1024U));
}
