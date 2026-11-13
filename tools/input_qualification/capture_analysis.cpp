#include "capture_analysis.hpp"

#include <GameInput.h>

#include "capture_report.hpp"
#include "gameinput_observation.hpp"
#include "report.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace inputnext::input_qualification::diagnostic {
namespace {

[[nodiscard]] std::vector<reading_view> index_callback_readings(
    const std::span<const reading_view> records) {
    std::vector<reading_view> index;
    index.reserve(records.size());
    for (const auto& record : records) {
        if (record.canonical_identity != 0U) {
            index.push_back(record);
        }
    }
    std::ranges::sort(index, {}, &reading_view::canonical_identity);
    return index;
}

void compare_native_record(const reading_view& native,
    const std::span<const reading_view> callback_index,
    const std::uint64_t unlabeled_mask,
    capture_overlap_analysis& result) {
    if (native.canonical_identity == 0U) {
        return;
    }
    const auto first = std::ranges::lower_bound(callback_index,
        native.canonical_identity,
        {},
        &reading_view::canonical_identity);
    if (first == callback_index.end() || first->canonical_identity != native.canonical_identity) {
        return;
    }
    ++result.native_records_seen_in_callback;
    const auto& native_sample = native.data.get();
    for (auto match = first;
        match != callback_index.end() && match->canonical_identity == native.canonical_identity;
        ++match) {
        const auto& callback = match->data.get();
        ++result.identity_pairs;
        if (native_sample.raw_touch.attempted && callback.raw_touch.attempted) {
            ++result.raw_touch_identity_pairs;
            if (inputnext::input_qualification::dualsense_usb_touch_payload_equal(
                    native_sample.raw_touch,
                    callback.raw_touch)) {
                ++result.raw_touch_payload_matches;
            } else {
                ++result.raw_touch_payload_mismatches;
            }
        }
        const bool payload_equal = reading_payload_equal(native_sample, callback);
        if (payload_equal) {
            ++result.payload_matches;
        } else {
            ++result.payload_mismatches;
        }
        const std::span<const std::uint32_t> surface_contact_counts{
            callback.returned_contact_counts.data(),
            callback.surface_count};
        const bool has_touch_contact =
            inputnext::input_qualification::has_any_touch_contacts(surface_contact_counts);
        const bool has_raw_touch_contact =
            callback.raw_touch.available && callback.raw_touch.contact_count != 0U;
        const bool gesture_bearing =
            ((native_sample.button_bits | callback.button_bits) & unlabeled_mask) != 0U ||
            has_touch_contact || has_raw_touch_contact;
        if (gesture_bearing) {
            ++result.gesture_bearing_identity_matches;
            if (payload_equal) {
                ++result.gesture_bearing_payload_matches;
            }
        }
    }
}

[[nodiscard]] capture_overlap_analysis compare_callback_native(
    const native_capture_view& native_capture,
    const std::span<const reading_view> callback_index,
    const std::uint64_t unlabeled_mask) {
    capture_overlap_analysis result{};
    for (const auto& native : native_capture.frames) {
        compare_native_record(native, callback_index, unlabeled_mask, result);
    }
    return result;
}

[[nodiscard]] std::uint64_t count_timestamp_collisions(
    std::vector<std::pair<std::uint64_t, std::uintptr_t>>& timestamp_identities) {
    std::ranges::sort(timestamp_identities);
    std::uint64_t collisions{};
    for (std::size_t begin = 0U; begin < timestamp_identities.size();) {
        auto end = begin + 1U;
        while (end < timestamp_identities.size() &&
               timestamp_identities[end].first == timestamp_identities[begin].first) {
            ++end;
        }
        const auto has_distinct_identity =
            end - begin > 1U && std::ranges::any_of(timestamp_identities.begin() +
                                                        static_cast<std::ptrdiff_t>(begin + 1U),
                                    timestamp_identities.begin() + static_cast<std::ptrdiff_t>(end),
                                    [first = timestamp_identities[begin].second](
                                        const auto& entry) { return entry.second != first; });
        collisions += has_distinct_identity ? 1U : 0U;
        begin = end;
    }
    return collisions;
}

[[nodiscard]] callback_capture_analysis inspect_callback_capture(
    const std::span<const reading_view> records) {
    callback_capture_analysis result{};
    std::vector<std::pair<std::uint64_t, std::uintptr_t>> timestamp_identities;
    timestamp_identities.reserve(records.size());
    for (std::size_t index = 0U; index < records.size(); ++index) {
        const auto& record = records[index];
        const auto timestamp = record.data.get().timestamp;
        if (index > 0U && records[index - 1U].data.get().timestamp > timestamp) {
            ++result.timestamp_regressions;
        }
        result.identity_failures += record.canonical_identity == 0U ? 1U : 0U;
        timestamp_identities.emplace_back(timestamp, record.canonical_identity);
    }
    result.timestamp_collisions = count_timestamp_collisions(timestamp_identities);
    return result;
}

void count_raw_touch(const inputnext::input_qualification::dualsense_usb_touch_record& observation,
    raw_touch_capture_analysis& counts) noexcept {
    if (!observation.attempted) {
        return;
    }
    ++counts.reads_attempted;
    counts.reads_available += observation.available ? 1U : 0U;
    counts.reads_missing += observation.available ? 0U : 1U;
    counts.click_pressed +=
        observation.click_state_available && observation.click_pressed ? 1U : 0U;
    counts.frames_with_contacts +=
        observation.available && observation.contact_count != 0U ? 1U : 0U;
    counts.contacts += observation.available ? observation.contact_count : 0U;
}

void inspect_input_activity(const device_report& device,
    const report_context& report,
    capture_analysis& analysis) {
    std::array<std::int32_t, max_button_count> labels{};
    for (std::uint32_t index = 0U; index < device.labels_returned; ++index) {
        labels[index] = static_cast<std::int32_t>(device.labels[index]);
    }
    std::vector<std::uint64_t> callback_button_states;
    callback_button_states.reserve(report.callbacks.records.size());
    for (const auto& callback : report.callbacks.records) {
        callback_button_states.push_back(callback.data.get().button_bits);
        count_raw_touch(callback.data.get().raw_touch, analysis.callback_raw_touch);
    }
    std::vector<std::uint64_t> native_button_states;
    native_button_states.reserve(report.native_forward.frames.size());
    for (const auto& native : report.native_forward.frames) {
        native_button_states.push_back(native.data.get().button_bits);
        count_raw_touch(native.data.get().raw_touch, analysis.native_raw_touch);
    }
    const std::span<const std::int32_t> label_view{labels.data(), device.labels_returned};
    const auto unlabeled_mask =
        inputnext::input_qualification::unlabeled_button_mask(device.button_count, label_view);
    const auto callback_index = index_callback_readings(report.callbacks.records);
    analysis.callback_unlabeled_activity =
        saw_unlabeled_button_transition(device.button_count, label_view, callback_button_states);
    analysis.native_unlabeled_activity =
        saw_unlabeled_button_transition(device.button_count, label_view, native_button_states);
    analysis.forward_overlap =
        compare_callback_native(report.native_forward, callback_index, unlabeled_mask);
    analysis.tail_overlap =
        compare_callback_native(report.native_tail, callback_index, unlabeled_mask);
}

[[nodiscard]] bool reading_fields_incomplete(const reading_sample& sample,
    const device_report& device) noexcept {
    return !inputnext::input_qualification::reading_fields_complete(sample,
        device.button_count,
        device.surface_count);
}

[[nodiscard]] bool device_fields_incomplete(const device_report& device) noexcept {
    if (device.button_count_truncated || device.surface_count_truncated) {
        return true;
    }
    for (std::uint32_t surface = 0U; surface < device.surface_count; ++surface) {
        if (!device.surface_info_available[surface]) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_touch_button_label(const device_report& device) noexcept {
    const auto labels_end =
        device.labels.begin() + static_cast<std::ptrdiff_t>(device.labels_returned);
    return std::ranges::find(device.labels.begin(),
               labels_end,
               gameinput::GameInputLabelTouchpadButton) != labels_end;
}

[[nodiscard]] qualification_capture_evidence make_qualification_evidence(
    const device_report& device,
    const report_context& report,
    const capture_analysis& analysis) {
    const auto& capture = report.callbacks;
    const auto& native_capture = report.native_forward;
    std::uint64_t callback_incomplete_records{};
    for (const auto& callback : capture.records) {
        callback_incomplete_records +=
            reading_fields_incomplete(callback.data.get(), device) ? 1U : 0U;
    }
    std::uint64_t native_incomplete_records{};
    for (const auto& native : native_capture.frames) {
        native_incomplete_records += reading_fields_incomplete(native.data.get(), device) ? 1U : 0U;
    }
    return qualification_capture_evidence{.raw_usb_requested = device.raw_usb_mode_requested,
        .raw_usb_enabled = device.raw_usb_mode_enabled,
        .has_touch_metadata = device.surface_count != 0U || has_touch_button_label(device),
        .button_labels_complete = device.labels_returned >= device.button_count,
        .descriptor_fields_complete = !device_fields_incomplete(device),
        .native_forward_complete = native_capture.forward_complete,
        .native_forward_truncated = native_capture.truncated,
        .callback_unlabeled_activity = analysis.callback_unlabeled_activity,
        .native_unlabeled_activity = analysis.native_unlabeled_activity,
        .callback_records = capture.records.size(),
        .callback_drops = capture.drops,
        .callback_incomplete_records = callback_incomplete_records,
        .callback_identity_failures = analysis.callbacks.identity_failures,
        .callback_timestamp_order_violations = analysis.callbacks.timestamp_regressions,
        .native_records = native_capture.frames.size(),
        .native_incomplete_records = native_incomplete_records,
        .native_identity_failures = native_capture.order.identity_failures,
        .native_timestamp_order_violations = native_capture.order.timestamp_order_violations,
        .identity_pairs = analysis.forward_overlap.identity_pairs,
        .payload_mismatches = analysis.forward_overlap.payload_mismatches,
        .gesture_payload_matches = analysis.forward_overlap.gesture_bearing_payload_matches,
        .callback_raw_reads_available = analysis.callback_raw_touch.reads_available,
        .callback_raw_reads_missing = analysis.callback_raw_touch.reads_missing,
        .native_raw_reads_available = analysis.native_raw_touch.reads_available,
        .native_raw_reads_missing = analysis.native_raw_touch.reads_missing,
        .raw_identity_pairs = analysis.forward_overlap.raw_touch_identity_pairs,
        .raw_payload_mismatches = analysis.forward_overlap.raw_touch_payload_mismatches};
}

} // namespace

capture_analysis analyze_capture(const report_context& report) {
    capture_analysis result{};
    if (report.device == nullptr) {
        result.qualification = "inconclusive_no_matching_device_seen";
        return result;
    }

    result.callbacks = inspect_callback_capture(report.callbacks.records);
    inspect_input_activity(*report.device, report, result);
    result.qualification =
        classify_capture_qualification(make_qualification_evidence(*report.device, report, result));
    return result;
}

} // namespace inputnext::input_qualification::diagnostic
