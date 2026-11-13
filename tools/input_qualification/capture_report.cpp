#include <GameInput.h>

#include "capture_analysis.hpp"
#include "capture_report.hpp"
#include "gameinput_observation.hpp"
#include "report.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <ostream>
#include <string>
#include <string_view>

namespace inputnext::input_qualification::diagnostic {
namespace {

constexpr auto observation_filter = static_cast<std::uint32_t>(observation_kinds);

[[nodiscard]] std::string label_name(gameinput::GameInputLabel label) {
    if (label == gameinput::GameInputLabelTouchpadButton) {
        return "touchpad_button";
    }
    if (label == gameinput::GameInputLabelXboxView || label == gameinput::GameInputLabelView ||
        label == gameinput::GameInputLabelSelect) {
        return "view_or_select";
    }
    if (label == gameinput::GameInputLabelXboxMenu || label == gameinput::GameInputLabelMenu ||
        label == gameinput::GameInputLabelStart) {
        return "menu_or_start";
    }
    if (label == gameinput::GameInputLabelUnknown) {
        return "unknown";
    }
    if (label == gameinput::GameInputLabelNone) {
        return "none";
    }
    return "label_" + std::to_string(static_cast<int>(label));
}

void write_sample(std::ostream& output,
    const reading_sample& sample,
    std::uintptr_t identity,
    const std::uint32_t order) {
    output << R"({"order":)" << order << R"(,"timestamp":)" << sample.timestamp
           << R"(,"input_kind":)" << sample.input_kind << R"(,"identity":)";
    inputnext::input_qualification::write_hex_json(output, static_cast<std::uint64_t>(identity));
    output << R"(,"gamepad_available":)" << (sample.gamepad_available ? "true" : "false")
           << R"(,"gamepad_buttons":)" << static_cast<std::uint32_t>(sample.gamepad_buttons)
           << R"(,"left_trigger":)";
    if (std::isfinite(sample.left_trigger)) {
        output << sample.left_trigger;
    } else {
        output << "null";
    }
    output << R"(,"right_trigger":)";
    if (std::isfinite(sample.right_trigger)) {
        output << sample.right_trigger;
    } else {
        output << "null";
    }
    output << R"(,"left_thumbstick_x":)";
    if (std::isfinite(sample.left_thumbstick_x)) {
        output << sample.left_thumbstick_x;
    } else {
        output << "null";
    }
    output << R"(,"left_thumbstick_y":)";
    if (std::isfinite(sample.left_thumbstick_y)) {
        output << sample.left_thumbstick_y;
    } else {
        output << "null";
    }
    output << R"(,"right_thumbstick_x":)";
    if (std::isfinite(sample.right_thumbstick_x)) {
        output << sample.right_thumbstick_x;
    } else {
        output << "null";
    }
    output << R"(,"right_thumbstick_y":)";
    if (std::isfinite(sample.right_thumbstick_y)) {
        output << sample.right_thumbstick_y;
    } else {
        output << "null";
    }
    output << R"(,"reading_button_count":)" << sample.reading_button_count
           << R"(,"returned_button_count":)" << sample.returned_button_count
           << R"(,"button_bits_hex":)";
    inputnext::input_qualification::write_hex_json(output, sample.button_bits);
    output << R"(,"surfaces":[)";
    for (std::uint32_t surface = 0U; surface < sample.surface_count; ++surface) {
        if (surface != 0U) {
            output << ',';
        }
        output << R"({"index":)" << surface << R"(,"reported_contacts":)"
               << sample.reported_contact_counts[surface] << R"(,"returned_contacts":)"
               << sample.returned_contact_counts[surface] << R"(,"contacts":[)";
        for (std::uint32_t contact = 0U; contact < sample.returned_contact_counts[surface];
            ++contact) {
            if (contact != 0U) {
                output << ',';
            }
            const auto& point = sample.contacts[surface][contact];
            output << R"({"id":)" << point.id << R"(,"x":)" << point.x << R"(,"y":)" << point.y
                   << R"(,"pad_x":)" << point.pad_x << R"(,"pad_y":)" << point.pad_y << '}';
        }
        output << "]}";
    }
    output << R"(],"button_count_truncated":)" << (sample.button_count_truncated ? "true" : "false")
           << R"(,"surface_count_truncated":)"
           << (sample.surface_count_truncated ? "true" : "false")
           << R"(,"contact_count_truncated":)"
           << (sample.contact_count_truncated ? "true" : "false");
    if (sample.raw_touch.attempted) {
        output << R"(,"dualsense_usb_raw_touch":)";
        inputnext::input_qualification::write_dualsense_usb_touch_json(output, sample.raw_touch);
    }
    output << '}';
}

void write_device_details(std::ostream& output, const device_report& device) {
    output << R"("device_status":"matching_device_observed",)"
           << R"("device":{"vid":")" << std::hex << std::setw(4) << std::setfill('0')
           << device.vendor << R"(","pid":")" << std::setw(4) << device.product << std::dec
           << R"(","revision":)" << device.revision << R"(,"device_id":")" << device.device_id
           << R"(","display_name":")"
           << inputnext::input_qualification::escape_json_string(device.display_name)
           << R"(","pnp_path":")"
           << inputnext::input_qualification::escape_json_string(device.pnp_path)
           << R"(","transport_api_value":null)"
           << R"(,"hardware_version":{"major":)" << device.hardware_version.major << R"(,"minor":)"
           << device.hardware_version.minor << R"(,"build":)" << device.hardware_version.build
           << R"(,"revision":)" << device.hardware_version.revision << '}'
           << R"(,"firmware_version":{"major":)" << device.firmware_version.major << R"(,"minor":)"
           << device.firmware_version.minor << R"(,"build":)" << device.firmware_version.build
           << R"(,"revision":)" << device.firmware_version.revision << '}'
           << R"(,"supported_input":)" << device.supported_input << R"(,"controller_button_count":)"
           << device.button_count << R"(,"labels_returned":)" << device.labels_returned
           << R"(,"button_count_truncated":)" << (device.button_count_truncated ? "true" : "false")
           << R"(,"button_labels":[)";
    for (std::uint32_t index = 0U; index < device.labels_returned; ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << R"({"index":)" << index << R"(,"value":)"
               << static_cast<int>(device.labels[index]) << R"(,"label":")"
               << label_name(device.labels[index]) << R"("})";
    }
    output << R"(],"touch_surface_count":)" << device.surface_count
           << R"(,"touch_surface_count_truncated":)"
           << (device.surface_count_truncated ? "true" : "false") << R"(,"touch_surfaces":[)";
    for (std::uint32_t index = 0U; index < device.surface_count; ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << R"({"info_available":)"
               << (device.surface_info_available[index] ? "true" : "false") << R"(,"type":)"
               << static_cast<int>(device.surfaces[index].type) << R"(,"max_contact_count":)"
               << device.surfaces[index].maxContactCount << R"(,"pad_width":)"
               << device.surfaces[index].padWidth << R"(,"pad_height":)"
               << device.surfaces[index].padHeight << '}';
    }
    output << ']';
    if (device.raw_usb_mode_requested) {
        output << R"(,"input_report_count":)" << device.input_report_count
               << R"(,"returned_input_report_count":)" << device.returned_input_report_count
               << R"(,"input_report_count_truncated":)"
               << (device.input_report_count_truncated ? "true" : "false")
               << R"(,"input_report_descriptors":[)";
        for (std::uint32_t index = 0U; index < device.returned_input_report_count; ++index) {
            if (index != 0U) {
                output << ',';
            }
            const auto& descriptor = device.input_reports[index];
            output << R"({"kind":)" << static_cast<int>(descriptor.kind) << R"(,"id":)"
                   << descriptor.id << R"(,"size":)" << descriptor.size << '}';
        }
        output << R"(],"raw_usb_mode_requested":true,"raw_usb_mode_enabled":)"
               << (device.raw_usb_mode_enabled ? "true" : "false");
    }
    output << "},\n";
}

void write_callback_capture(std::ostream& output,
    const callback_capture_view& capture,
    const callback_capture_analysis& summary) {
    output << R"("callback_capture":{"filter_input_kind":)" << observation_filter
           << R"(,"samples":)" << capture.records.size() << R"(,"record_capacity":)"
           << max_callback_records << R"(,"truncated":)" << (capture.drops != 0U ? "true" : "false")
           << R"(,"dropped":)" << capture.drops << R"(,"missing_gamepad_getter":)"
           << capture.missing_gamepad << R"(,"short_controller_state":)"
           << capture.short_button_state << R"(,"contacts_over_cap":)" << capture.truncated_contacts
           << R"(,"same_timestamp_distinct_readings":)" << summary.timestamp_collisions
           << R"(,"timestamp_order_violations":)" << summary.timestamp_regressions
           << R"(,"identity_failures":)" << summary.identity_failures << R"(,"records":[)";
    for (std::uint32_t index = 0U; index < capture.records.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        const auto& record = capture.records[index];
        write_sample(output, record.data.get(), record.canonical_identity, index);
    }
    output << "]},\n";
}

void write_activity_summary(std::ostream& output, const capture_analysis& analysis) {
    output << R"("unlabeled_button_activity":{"callback":)"
           << (analysis.callback_unlabeled_activity ? "true" : "false") << R"(,"native":)"
           << (analysis.native_unlabeled_activity ? "true" : "false") << "},\n";
}

void write_raw_touch_source_counts(std::ostream& output, const raw_touch_capture_analysis& counts) {
    output << R"({"reads_attempted":)" << counts.reads_attempted << R"(,"reads_available":)"
           << counts.reads_available << R"(,"reads_missing":)" << counts.reads_missing
           << R"(,"click_pressed":)" << counts.click_pressed << R"(,"frames_with_contacts":)"
           << counts.frames_with_contacts << R"(,"contacts":)" << counts.contacts << '}';
}

void write_raw_touch_diagnostics(std::ostream& output,
    const device_report& device,
    const capture_analysis& analysis) {
    if (!device.raw_usb_mode_requested) {
        return;
    }
    const bool unique_report =
        device.input_report_count == 1U && device.returned_input_report_count == 1U &&
        !device.input_report_count_truncated && device.matching_raw_input_descriptor_count == 1U;
    output << R"("dualsense_usb_raw_touch":{"requested":true,"enabled":)"
           << (device.raw_usb_mode_enabled ? "true" : "false") << R"(,"status":")"
           << (device.raw_usb_mode_enabled ? "enabled_experimental_unqualified"
                                           : "disabled_layout_or_report_descriptor_unverified")
           << R"(","click_button_index":)" << dualsense_usb_click_button
           << R"(,"click_layout_valid":)"
           << (device.button_count == dualsense_usb_button_count ? "true" : "false")
           << R"(,"input_report_count":)" << device.input_report_count
           << R"(,"returned_input_report_count":)" << device.returned_input_report_count
           << R"(,"matching_report_descriptor_count":)"
           << device.matching_raw_input_descriptor_count << R"(,"unique_expected_report":)"
           << (unique_report ? "true" : "false") << R"(,"callback":)";
    write_raw_touch_source_counts(output, analysis.callback_raw_touch);
    output << R"(,"native_forward":)";
    write_raw_touch_source_counts(output, analysis.native_raw_touch);
    output << R"(,"forward_identity_pairs":)" << analysis.forward_overlap.raw_touch_identity_pairs
           << R"(,"forward_payload_matches":)" << analysis.forward_overlap.raw_touch_payload_matches
           << R"(,"forward_payload_mismatches":)"
           << analysis.forward_overlap.raw_touch_payload_mismatches << "},\n";
}

void write_native_history(std::ostream& output,
    const native_capture_view& capture,
    const native_capture_view& tail,
    const capture_analysis& analysis) {
    const inputnext::input_qualification::native_capture_summary capture_summary{
        .status = "forward_capture_stream",
        .filter_input_kind = observation_filter,
        .records = capture.frames.size(),
        .record_capacity = max_native_capture_records,
        .truncated = capture.truncated,
        .identity_failures = capture.order.identity_failures,
        .transition_count = capture.transitions,
        .stop_reason = capture.stop_reason,
        .seed_hresult = static_cast<std::int32_t>(capture.seed_hresult),
        .last_hresult = static_cast<std::int32_t>(capture.last_hresult),
        .no_next_polls = capture.no_next_polls,
        .next_stop_reason = "not_applicable_forward_walk",
        .callback_native_identity_matches = analysis.forward_overlap.identity_pairs,
        .native_records_seen_in_callback = analysis.forward_overlap.native_records_seen_in_callback,
        .payload_matches = analysis.forward_overlap.payload_matches,
        .payload_mismatches = analysis.forward_overlap.payload_mismatches,
        .gesture_bearing_identity_matches =
            analysis.forward_overlap.gesture_bearing_identity_matches,
        .gesture_bearing_payload_matches = analysis.forward_overlap.gesture_bearing_payload_matches,
        .same_timestamp_distinct_readings = capture.order.same_timestamp_distinct_readings,
        .timestamp_order_violations = capture.order.timestamp_order_violations};
    output << R"("native_capture":)";
    inputnext::input_qualification::write_native_capture_prefix(output, capture_summary);
    for (std::size_t index = 0U; index < capture.frames.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        write_sample(output,
            capture.frames[index].data.get(),
            capture.frames[index].canonical_identity,
            static_cast<std::uint32_t>(index));
    }

    const inputnext::input_qualification::native_capture_summary tail_summary{
        .status = "post_capture_backward_tail_diagnostic_only",
        .filter_input_kind = observation_filter,
        .records = tail.frames.size(),
        .record_capacity = max_native_tail_records,
        .truncated = tail.truncated,
        .identity_failures = tail.order.identity_failures,
        .transition_count = tail.transitions,
        .stop_reason = tail.stop_reason,
        .seed_hresult = static_cast<std::int32_t>(tail.seed_hresult),
        .last_hresult = static_cast<std::int32_t>(tail.last_hresult),
        .next_stop_reason = "not_applicable_backward_tail",
        .callback_native_identity_matches = analysis.tail_overlap.identity_pairs,
        .native_records_seen_in_callback = analysis.tail_overlap.native_records_seen_in_callback,
        .payload_matches = analysis.tail_overlap.payload_matches,
        .payload_mismatches = analysis.tail_overlap.payload_mismatches,
        .gesture_bearing_identity_matches = analysis.tail_overlap.gesture_bearing_identity_matches,
        .gesture_bearing_payload_matches = analysis.tail_overlap.gesture_bearing_payload_matches,
        .same_timestamp_distinct_readings = tail.order.same_timestamp_distinct_readings,
        .timestamp_order_violations = tail.order.timestamp_order_violations};
    output << "]},\n\"native_tail_diagnostic\":";
    inputnext::input_qualification::write_native_capture_prefix(output, tail_summary);
    for (std::size_t index = 0U; index < tail.frames.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        write_sample(output,
            tail.frames[index].data.get(),
            tail.frames[index].canonical_identity,
            static_cast<std::uint32_t>(index));
    }
}

void write_report_header(std::ostream& output, const report_context& report) {
    output << R"({
"schema":"inputnext.gameinput-qualification.v1",
"status":"observational_only",
"runtime_sdk_version":")"
           << report.sdk_version << R"(",
"runtime_file_version":")"
           << inputnext::input_qualification::escape_json_string(report.runtime_file_version)
           << R"(",
"runtime_path":")"
           << inputnext::input_qualification::escape_json_string(report.runtime_path) << R"(",
"process_architecture":")"
           << report.architecture << R"(",
"capture_seconds":)"
           << report.capture_seconds << R"(,
"target_filter":{"vid":"054c","pid":"0ce6"},
"instructions_shown":true,
"registration_errors":)"
           << report.registration_errors << R"(,
"device_event_drops":)"
           << report.device_event_drops;
    if (report.raw_usb_requested) {
        output << R"(,"dualsense_usb_raw_requested":true)";
    }
    output << ",\n";
}

void write_empty_device_report(std::ostream& output,
    const inputnext::input_qualification::bootstrap_measurement_summary& bootstrap_summary) {
    output << R"("device_status":"not_observed",
"callback_capture":{"samples":0,"drops":0},
"native_capture":{"status":"not_run_without_target"},
"qualification":"inconclusive_no_matching_device_seen",
"bootstrap_measurement":)";
    inputnext::input_qualification::write_bootstrap_measurement_json(output, bootstrap_summary);
    output << "\n}\n";
}

} // namespace

void write_capture_report(std::ostream& output, const report_context& report) {
    const auto analysis = analyze_capture(report);
    write_report_header(output, report);
    if (report.device == nullptr) {
        write_empty_device_report(output, report.bootstrap);
        return;
    }
    const auto& device = *report.device;
    write_device_details(output, device);
    write_callback_capture(output, report.callbacks, analysis.callbacks);
    write_activity_summary(output, analysis);
    write_raw_touch_diagnostics(output, device, analysis);
    write_native_history(output, report.native_forward, report.native_tail, analysis);
    output << "]},\n\"qualification\":\"";
    output << analysis.qualification << "\"";
    output << ",\"bootstrap_measurement\":";
    inputnext::input_qualification::write_bootstrap_measurement_json(output, report.bootstrap);
    output << "\n}\n";
}

} // namespace inputnext::input_qualification::diagnostic
