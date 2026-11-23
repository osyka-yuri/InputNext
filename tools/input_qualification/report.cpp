#include "report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace inputnext::input_qualification {
namespace {

void write_json_string(std::ostream& output, const std::string_view value) {
    output << '"' << escape_json_string(value) << '"';
}

class stream_flags_restore final {
public:
    explicit stream_flags_restore(std::ostream& stream) noexcept
        : stream_(stream),
          flags_(stream.flags()) {}

    ~stream_flags_restore() noexcept { stream_.flags(flags_); }

    stream_flags_restore(const stream_flags_restore&) = delete;
    stream_flags_restore& operator=(const stream_flags_restore&) = delete;
    stream_flags_restore(stream_flags_restore&&) = delete;
    stream_flags_restore& operator=(stream_flags_restore&&) = delete;

private:
    std::ostream& stream_;
    const std::ios_base::fmtflags flags_;
};

[[nodiscard]] std::uint64_t ticks_to_microseconds(const std::uint64_t ticks,
    const std::uint64_t frequency) noexcept {
    if (frequency == 0U) {
        return 0U;
    }
    const auto microseconds =
        static_cast<long double>(ticks) * 1'000'000.0L / static_cast<long double>(frequency);
    const auto maximum = static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    return microseconds >= maximum ? std::numeric_limits<std::uint64_t>::max()
                                   : static_cast<std::uint64_t>(microseconds);
}

struct duration_statistics final {
    std::size_t count{};
    std::uint64_t p50{};
    std::uint64_t p95{};
    std::uint64_t p99{};
    std::uint64_t maximum{};
};

struct bootstrap_attempt_output_context final {
    std::uint64_t qpc_frequency{};
    std::size_t index{};
};

[[nodiscard]] duration_statistics summarize_duration(
    const std::span<const bootstrap_attempt_summary> attempts,
    const std::uint64_t qpc_frequency,
    const std::uint64_t bootstrap_attempt_summary::* duration,
    const bool bootstrap_attempt_summary::* measured) noexcept {
    std::array<std::uint64_t, 1024U> values{};
    std::size_t count{};
    for (const auto& attempt : attempts) {
        if (attempt.timing_available && attempt.*measured && count < values.size()) {
            values[count] = ticks_to_microseconds(attempt.*duration, qpc_frequency);
            ++count;
        }
    }
    if (count == 0U) {
        return {};
    }
    std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    const auto nearest_rank = [count](const std::uint64_t percentile) noexcept {
        const auto rank = ((static_cast<std::uint64_t>(count) * percentile) + 99U) / 100U;
        return static_cast<std::size_t>(rank == 0U ? 0U : rank - 1U);
    };
    return duration_statistics{.count = count,
        .p50 = values[nearest_rank(50U)],
        .p95 = values[nearest_rank(95U)],
        .p99 = values[nearest_rank(99U)],
        .maximum = values[count - 1U]};
}

void write_duration_statistics(std::ostream& output,
    const std::string_view name,
    const duration_statistics& statistics) {
    output << '"' << name << R"(_us":{"count":)" << statistics.count << R"(,"p50":)";
    if (statistics.count == 0U) {
        output << R"(null,"p95":null,"p99":null,"max":null})";
        return;
    }
    output << statistics.p50 << R"(,"p95":)" << statistics.p95 << R"(,"p99":)" << statistics.p99
           << R"(,"max":)" << statistics.maximum << '}';
}

void write_timing_summary(std::ostream& output, const bootstrap_measurement_summary& summary) {
    output << R"("timing_summary":{)";
    write_duration_statistics(output,
        "factory",
        summarize_duration(summary.attempts,
            summary.qpc_frequency,
            &bootstrap_attempt_summary::factory_ticks,
            &bootstrap_attempt_summary::factory_measured));
    output << ',';
    write_duration_statistics(output,
        "start",
        summarize_duration(summary.attempts,
            summary.qpc_frequency,
            &bootstrap_attempt_summary::start_ticks,
            &bootstrap_attempt_summary::start_measured));
    output << ',';
    write_duration_statistics(output,
        "destruction",
        summarize_duration(summary.attempts,
            summary.qpc_frequency,
            &bootstrap_attempt_summary::destruction_ticks,
            &bootstrap_attempt_summary::destruction_measured));
    output << ',';
    write_duration_statistics(output,
        "total",
        summarize_duration(summary.attempts,
            summary.qpc_frequency,
            &bootstrap_attempt_summary::total_ticks,
            &bootstrap_attempt_summary::total_measured));
    output << '}';
}

void write_fallback_counts(std::ostream& output,
    const std::span<const bootstrap_attempt_summary> attempts) {
    constexpr std::array<std::string_view, 15U> reasons{"unsupported_capability",
        "incoherent_reading",
        "missing_click",
        "invalid_coordinates",
        "history_lost",
        "incomplete_replay",
        "bootstrap_budget",
        "epoch_changed",
        "native_failure",
        "no_current_reading",
        "device_disconnected",
        "clock_discontinuity",
        "api_unavailable",
        "resource_exhausted",
        "registration_failed"};
    output << R"("fallback_by_reason":{)";
    bool first_reason = true;
    for (const auto reason : reasons) {
        const auto count = std::count_if(attempts.begin(),
            attempts.end(),
            [reason](const bootstrap_attempt_summary& attempt) {
                return attempt.fallback && attempt.reason == reason;
            });
        if (count == 0) {
            continue;
        }
        if (!first_reason) {
            output << ',';
        }
        first_reason = false;
        write_json_string(output, reason);
        output << ':' << count;
    }
    output << "},";
}

void write_attempt_duration(std::ostream& output,
    const bootstrap_attempt_summary& attempt,
    const std::uint64_t qpc_frequency,
    const std::string_view name,
    const std::uint64_t bootstrap_attempt_summary::* ticks,
    const bool bootstrap_attempt_summary::* measured) {
    output << '"' << name << R"(_us":)";
    if (!attempt.timing_available || !(attempt.*measured)) {
        output << "null";
        return;
    }
    output << ticks_to_microseconds(attempt.*ticks, qpc_frequency);
}

void write_bootstrap_attempt(std::ostream& output,
    const bootstrap_attempt_summary& attempt,
    const bootstrap_attempt_output_context context) {
    output << R"({"index":)" << context.index << R"(,"timing_available":)"
           << (attempt.timing_available ? "true" : "false") << ',';
    write_attempt_duration(output,
        attempt,
        context.qpc_frequency,
        "factory",
        &bootstrap_attempt_summary::factory_ticks,
        &bootstrap_attempt_summary::factory_measured);
    output << ',';
    write_attempt_duration(output,
        attempt,
        context.qpc_frequency,
        "start",
        &bootstrap_attempt_summary::start_ticks,
        &bootstrap_attempt_summary::start_measured);
    output << ',';
    write_attempt_duration(output,
        attempt,
        context.qpc_frequency,
        "destruction",
        &bootstrap_attempt_summary::destruction_ticks,
        &bootstrap_attempt_summary::destruction_measured);
    output << ',';
    write_attempt_duration(output,
        attempt,
        context.qpc_frequency,
        "total",
        &bootstrap_attempt_summary::total_ticks,
        &bootstrap_attempt_summary::total_measured);
    output << R"(,"bootstrap_transitions":)" << attempt.transitions << R"(,"succeeded":)"
           << (attempt.succeeded ? "true" : "false") << R"(,"fallback":)"
           << (attempt.fallback ? "true" : "false") << R"(,"native_code":)" << attempt.native_code
           << R"(,"reason":)";
    write_json_string(output, attempt.reason);
    output << R"(,"touch_source":)";
    write_json_string(output, attempt.touch_source);
    output << R"(,"touch_reason":)";
    write_json_string(output, attempt.touch_reason);
    output << R"(,"context":{"baseline_available":)"
           << (attempt.context_baseline_available ? "true" : "false") << R"(,"click_pressed":)"
           << (attempt.context_click_pressed ? "true" : "false") << R"(,"awaiting_release":)"
           << (attempt.context_awaiting_release ? "true" : "false") << R"(,"epoch":)"
           << attempt.context_epoch << R"(,"first_accepted_timestamp":)";
    if (attempt.context_has_first_timestamp) {
        output << attempt.context_first_timestamp;
    } else {
        output << "null";
    }
    output << "}}";
}

void write_attempts(std::ostream& output,
    const std::span<const bootstrap_attempt_summary> attempts,
    const std::uint64_t qpc_frequency) {
    output << R"("attempts":[)";
    for (std::size_t index = 0U; index < attempts.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        write_bootstrap_attempt(output,
            attempts[index],
            bootstrap_attempt_output_context{.qpc_frequency = qpc_frequency, .index = index});
    }
    output << "]}";
}

} // namespace

std::string escape_json_string(const std::string_view value) {
    constexpr std::string_view hex_digits{"0123456789abcdef"};
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
            escaped.push_back(character);
        } else if (byte < 0x20U) {
            escaped.append("\\u00");
            escaped.push_back(hex_digits[static_cast<std::size_t>(byte >> 4U)]);
            escaped.push_back(hex_digits[static_cast<std::size_t>(byte & 0x0FU)]);
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

void write_hex_json(std::ostream& output, const std::uint64_t value) {
    const stream_flags_restore restore{output};

    output << R"("0x)" << std::hex << std::nouppercase << value << '"';
}

void write_dualsense_usb_touch_json(std::ostream& output, const dualsense_usb_touch_record& touch) {
    output << R"({"attempted":)" << (touch.attempted ? "true" : "false")
           << R"(,"click_state_available":)" << (touch.click_state_available ? "true" : "false")
           << R"(,"click_pressed":)" << (touch.click_pressed ? "true" : "false")
           << R"(,"available":)" << (touch.available ? "true" : "false") << R"(,"resolved_side":")";
    if (!touch.side_evaluated) {
        output << "not_evaluated";
    } else {
        switch (touch.resolved_side) {
        case dualsense_usb_touch_side::none:
            output << "none";
            break;
        case dualsense_usb_touch_side::left:
            output << "left";
            break;
        case dualsense_usb_touch_side::right:
            output << "right";
            break;
        case dualsense_usb_touch_side::fallback:
            output << "fallback";
            break;
        }
    }
    output << R"(","contact_count":)" << touch.contact_count << R"(,"contacts":[)";
    for (std::uint32_t index = 0U; index < touch.contact_count; ++index) {
        if (index != 0U) {
            output << ',';
        }
        const auto& contact = touch.contacts[index];
        output << R"({"id":)" << contact.id << R"(,"x":)" << contact.x << R"(,"y":)" << contact.y
               << '}';
    }
    output << "]}";
}

bool dualsense_usb_touch_payload_equal(const dualsense_usb_touch_record& left,
    const dualsense_usb_touch_record& right) noexcept {
    if (left.attempted != right.attempted ||
        left.click_state_available != right.click_state_available ||
        left.click_pressed != right.click_pressed || left.available != right.available ||
        left.contact_count != right.contact_count) {
        return false;
    }
    if (!left.available) {
        return true;
    }
    for (std::uint32_t index = 0U; index < left.contact_count; ++index) {
        const auto& left_contact = left.contacts[index];
        const auto& right_contact = right.contacts[index];
        if (left_contact.id != right_contact.id || left_contact.x != right_contact.x ||
            left_contact.y != right_contact.y) {
            return false;
        }
    }
    return true;
}

bool reading_payload_equal(const reading_sample& left, const reading_sample& right) noexcept {
    if (left.timestamp != right.timestamp || left.input_kind != right.input_kind ||
        left.gamepad_available != right.gamepad_available ||
        left.reading_button_count != right.reading_button_count ||
        left.returned_button_count != right.returned_button_count ||
        left.button_bits != right.button_bits || left.surface_count != right.surface_count ||
        left.button_count_truncated != right.button_count_truncated ||
        left.surface_count_truncated != right.surface_count_truncated ||
        left.contact_count_truncated != right.contact_count_truncated ||
        !dualsense_usb_touch_payload_equal(left.raw_touch, right.raw_touch) ||
        left.surface_count > reading_surface_capacity) {
        return false;
    }
    const auto same_float = [](const float left_value, const float right_value) {
        return std::bit_cast<std::uint32_t>(left_value) ==
               std::bit_cast<std::uint32_t>(right_value);
    };
    if (left.gamepad_available &&
        (left.gamepad_buttons != right.gamepad_buttons ||
            !same_float(left.left_trigger, right.left_trigger) ||
            !same_float(left.right_trigger, right.right_trigger) ||
            !same_float(left.left_thumbstick_x, right.left_thumbstick_x) ||
            !same_float(left.left_thumbstick_y, right.left_thumbstick_y) ||
            !same_float(left.right_thumbstick_x, right.right_thumbstick_x) ||
            !same_float(left.right_thumbstick_y, right.right_thumbstick_y))) {
        return false;
    }
    for (std::uint32_t surface = 0U; surface < left.surface_count; ++surface) {
        const auto returned = left.returned_contact_counts[surface];
        if (left.reported_contact_counts[surface] != right.reported_contact_counts[surface] ||
            returned != right.returned_contact_counts[surface] ||
            returned > reading_contact_capacity) {
            return false;
        }
        for (std::uint32_t contact = 0U; contact < returned; ++contact) {
            const auto& left_contact = left.contacts[surface][contact];
            const auto& right_contact = right.contacts[surface][contact];
            if (left_contact.id != right_contact.id || left_contact.x != right_contact.x ||
                left_contact.y != right_contact.y || left_contact.pad_x != right_contact.pad_x ||
                left_contact.pad_y != right_contact.pad_y) {
                return false;
            }
        }
    }
    return true;
}

bool reading_fields_complete(const reading_sample& reading,
    const std::uint32_t descriptor_button_count,
    const std::uint32_t descriptor_surface_count) noexcept {
    if (!reading.gamepad_available || reading.button_count_truncated ||
        reading.reading_button_count != descriptor_button_count ||
        reading.returned_button_count != descriptor_button_count ||
        reading.surface_count_truncated || reading.surface_count != descriptor_surface_count ||
        reading.contact_count_truncated || descriptor_surface_count > reading_surface_capacity) {
        return false;
    }
    for (std::uint32_t surface = 0U; surface < descriptor_surface_count; ++surface) {
        const auto expected = std::min<std::uint32_t>(reading.reported_contact_counts[surface],
            static_cast<std::uint32_t>(reading_contact_capacity));
        if (reading.returned_contact_counts[surface] != expected) {
            return false;
        }
    }
    return true;
}

void write_native_capture_prefix(std::ostream& output, const native_capture_summary& summary) {
    output << R"({"status":)";
    write_json_string(output, summary.status);
    output << R"(,"filter_input_kind":)" << summary.filter_input_kind << R"(,"records":)"
           << summary.records << R"(,"record_capacity":)" << summary.record_capacity
           << R"(,"truncated":)" << (summary.truncated ? "true" : "false")
           << R"(,"identity_failures":)" << summary.identity_failures << R"(,"transition_count":)"
           << summary.transition_count << R"(,"stop_reason":)";
    write_json_string(output, summary.stop_reason);
    output << R"(,"seed_hresult":)" << summary.seed_hresult << R"(,"last_hresult":)"
           << summary.last_hresult << R"(,"no_next_polls":)" << summary.no_next_polls
           << R"(,"next_stop_reason":)";
    write_json_string(output, summary.next_stop_reason);
    output << R"(,"next_transitions":)" << summary.next_transitions
           << R"(,"next_identity_matches":)" << summary.next_identity_matches
           << R"(,"same_timestamp_distinct_readings":)" << summary.same_timestamp_distinct_readings
           << R"(,"timestamp_order_violations":)" << summary.timestamp_order_violations
           << R"(,"callback_native_identity_matches":)" << summary.callback_native_identity_matches
           << R"(,"native_records_seen_in_callback":)" << summary.native_records_seen_in_callback
           << R"(,"payload_matches":)" << summary.payload_matches << R"(,"payload_mismatches":)"
           << summary.payload_mismatches << R"(,"gesture_bearing_identity_matches":)"
           << summary.gesture_bearing_identity_matches << R"(,"gesture_bearing_payload_matches":)"
           << summary.gesture_bearing_payload_matches << R"(,"records_detail":[)";
}

void write_bootstrap_measurement_json(std::ostream& output,
    const bootstrap_measurement_summary& summary) {
    const auto succeeded = std::count_if(summary.attempts.begin(),
        summary.attempts.end(),
        [](const bootstrap_attempt_summary& attempt) { return attempt.succeeded; });
    const auto fallback = std::count_if(summary.attempts.begin(),
        summary.attempts.end(),
        [](const bootstrap_attempt_summary& attempt) { return attempt.fallback; });
    output << R"({"requested":)" << (summary.requested ? "true" : "false") << R"(,"mode":")"
           << (summary.native_surface_coordinates
                      ? "experimental_unqualified_composite_with_native_touch_coordinates"
                      : "experimental_unqualified_composite_without_coordinates")
           << R"(","status":)";
    write_json_string(output, summary.status);
    output << R"(,"click_button":)" << summary.click_button << R"(,"observed_button_count":)"
           << summary.observed_button_count << R"(,"click_index_valid":)"
           << (summary.click_index_valid ? "true" : "false") << R"(,"source_baseline_observed":)"
           << (summary.source_baseline_observed ? "true" : "false")
           << R"(,"time_source":"QueryPerformanceCounter","qpc_frequency":)"
           << summary.qpc_frequency << R"(,"capacity":)" << bootstrap_attempt_capacity
           << R"(,"attempt_count":)" << summary.attempts.size() << R"(,"truncated":)"
           << (summary.truncated ? "true" : "false") << R"(,"succeeded":)" << succeeded
           << R"(,"fallback_or_failed":)" << fallback
           << R"(,"timing_includes":["factory","start","destruction"],)";
    write_timing_summary(output, summary);
    output << ',';
    write_fallback_counts(output, summary.attempts);
    write_attempts(output, summary.attempts, summary.qpc_frequency);
}

bool bootstrap_attempt_capacity_available(const std::size_t attempt_count) noexcept {
    return attempt_count < bootstrap_attempt_capacity;
}

std::uint64_t unlabeled_button_mask(const std::uint32_t device_button_count,
    const std::span<const std::int32_t> labels) noexcept {
    const auto bounded_count = std::min(device_button_count, 64U);
    std::uint64_t unlabeled_mask{};
    for (std::uint32_t index = 0U; index < bounded_count; ++index) {
        if (index >= labels.size() || labels[index] == 0 || labels[index] == -1) {
            unlabeled_mask |= std::uint64_t{1U} << index;
        }
    }
    return unlabeled_mask;
}

bool saw_unlabeled_button_transition(const std::uint32_t device_button_count,
    const std::span<const std::int32_t> labels,
    const std::span<const std::uint64_t> state_masks) noexcept {
    const auto unlabeled_mask = unlabeled_button_mask(device_button_count, labels);
    for (std::size_t index = 1U; index < state_masks.size(); ++index) {
        if (((state_masks[index - 1U] ^ state_masks[index]) & unlabeled_mask) != 0U) {
            return true;
        }
    }
    return false;
}

bool has_any_touch_contacts(const std::span<const std::uint32_t> contact_counts) noexcept {
    return std::ranges::any_of(contact_counts,
        [](const std::uint32_t count) { return count != 0U; });
}

std::string_view classify_capture_qualification(
    const qualification_capture_evidence& evidence) noexcept {
    const bool callback_complete =
        evidence.callback_records != 0U && evidence.callback_drops == 0U &&
        evidence.callback_incomplete_records == 0U && evidence.callback_identity_failures == 0U &&
        evidence.callback_timestamp_order_violations == 0U;
    const bool native_complete =
        evidence.native_forward_complete && !evidence.native_forward_truncated &&
        evidence.native_records != 0U && evidence.native_incomplete_records == 0U &&
        evidence.native_identity_failures == 0U && evidence.native_timestamp_order_violations == 0U;
    const bool identity_payload_overlap =
        evidence.identity_pairs != 0U && evidence.payload_mismatches == 0U;
    if (!evidence.descriptor_fields_complete || !callback_complete || !native_complete ||
        !identity_payload_overlap) {
        return evidence.raw_usb_requested
                   ? "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required"
                   : "inconclusive_forward_native_coverage_or_gesture_overlap";
    }

    if (evidence.raw_usb_requested) {
        if (!evidence.raw_usb_enabled) {
            return "raw_usb_touch_unavailable_unverified_button_layout_or_report_descriptor";
        }
        if (evidence.callback_raw_reads_available == 0U ||
            evidence.callback_raw_reads_missing != 0U ||
            evidence.native_raw_reads_available == 0U || evidence.native_raw_reads_missing != 0U ||
            evidence.raw_identity_pairs == 0U || evidence.raw_payload_mismatches != 0U) {
            return "raw_usb_touch_capture_incomplete_or_incoherent_manual_review_required";
        }
        return "raw_usb_touch_observations_captured_manual_review_required_no_auto_qualification";
    }

    const bool observed_unlabeled_activity =
        evidence.callback_unlabeled_activity || evidence.native_unlabeled_activity;
    if (!evidence.has_touch_metadata) {
        if (!evidence.button_labels_complete) {
            return "inconclusive_touch_metadata_labels_incomplete";
        }
        if (!observed_unlabeled_activity) {
            return "touch_metadata_not_exposed_no_unlabeled_change_observed_manual_review_required";
        }
        return evidence.gesture_payload_matches != 0U
                   ? "unlabeled_button_change_observed_manual_review_required"
                   : "inconclusive_forward_native_coverage_or_gesture_overlap";
    }

    if (evidence.gesture_payload_matches == 0U) {
        return "inconclusive_forward_native_coverage_or_gesture_overlap";
    }
    return "observations_captured_manual_review_required_no_auto_qualification";
}

} // namespace inputnext::input_qualification
