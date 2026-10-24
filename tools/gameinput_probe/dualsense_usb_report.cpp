#include "dualsense_usb_report.hpp"

#include "dualsense_usb_evidence.hpp"
#include "rumble_report.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <iomanip>
#include <ios>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] std::string_view trigger_mode_name(const output::trigger_mode mode) noexcept {
    switch (mode) {
    case output::trigger_mode::off:
        return "off";
    case output::trigger_mode::feedback:
        return "feedback";
    case output::trigger_mode::weapon:
        return "weapon";
    case output::trigger_mode::vibration:
        return "vibration";
    }
    return "unknown";
}

[[nodiscard]] std::string_view trigger_layout_name(const output::trigger_layout layout) noexcept {
    switch (layout) {
    case output::trigger_layout::uniform:
        return "uniform";
    case output::trigger_layout::per_zone:
        return "per_zone";
    }
    return "unknown";
}

void append_values(std::ostream& stream, const output::native_output& native) {
    const auto& values = native.values;
    stream << " channels=0x" << std::hex << native.channels << std::dec << std::fixed
           << std::setprecision(3) << " rumble=" << values.rumble.low_frequency << ','
           << values.rumble.high_frequency
           << " left_trigger=" << trigger_mode_name(values.left_trigger.mode) << '/'
           << trigger_layout_name(values.left_trigger.layout)
           << " right_trigger=" << trigger_mode_name(values.right_trigger.mode) << '/'
           << trigger_layout_name(values.right_trigger.layout)
           << " leds=" << static_cast<unsigned int>(values.leds.rgb[0U]) << ','
           << static_cast<unsigned int>(values.leds.rgb[1U]) << ','
           << static_cast<unsigned int>(values.leds.rgb[2U]) << ':'
           << static_cast<unsigned int>(values.leds.player_leds) << ':'
           << (values.leds.microphone_led ? "on" : "off");
}

} // namespace

std::string_view dualsense_usb_evidence_name(const dualsense_usb_evidence evidence) noexcept {
    switch (evidence) {
    case dualsense_usb_evidence::pending:
        return "pending";
    case dualsense_usb_evidence::accepted_newer_attempt:
        return "accepted_newer_attempt";
    case dualsense_usb_evidence::historical_reuse:
        return "historical_reuse";
    case dualsense_usb_evidence::rejected:
        return "rejected";
    }
    return "unknown";
}

std::string_view dualsense_usb_status_name(const output::native_status status) noexcept {
    switch (status) {
    case output::native_status::accepted:
        return "accepted";
    case output::native_status::retryable_failure:
        return "retryable_failure";
    case output::native_status::unsupported:
        return "unsupported";
    case output::native_status::failed:
        return "failed";
    case output::native_status::disconnected:
        return "disconnected";
    }
    return "unknown";
}

std::string format_dualsense_usb_step(const dualsense_usb_step_report& report) {
    std::ostringstream stream;
    stream << "dualsense_usb_step=" << report.step
           << " target=" << format_rumble_target(report.target) << " number=" << report.step_number
           << '/' << report.step_count << " context=" << report.context.id.value
           << " sequence=" << report.accepted_sequence
           << " logical_version=" << report.logical_version
           << " foreground=" << (report.foreground ? "true" : "false")
           << " evidence=" << dualsense_usb_evidence_name(report.evidence);
    if (report.match.has_value()) {
        const auto& match = *report.match;
        stream << " backend=" << match.backend.incarnation.id.value
               << " publication=" << match.publication.value << " attempt=" << match.attempt.value
               << " native_status=" << dualsense_usb_status_name(match.result.status)
               << " native_code=" << match.result.native_code;
        append_values(stream, match.native_output);
    } else {
        stream << " backend=unconfirmed native_status=unconfirmed";
    }
    return stream.str();
}

std::string format_dualsense_usb_progress(const std::optional<output::backend_progress>& progress) {
    std::ostringstream stream;
    stream << "dualsense_usb_delivery";
    if (!progress.has_value()) {
        stream << " backend=unavailable native_status=unconfirmed";
        return stream.str();
    }
    stream << " backend=" << progress->backend.incarnation.id.value
           << " publication=" << progress->observed_publication.value
           << " in_flight=" << (progress->write_in_flight ? "true" : "false")
           << " retryable_failures=" << progress->consecutive_retryable_failures
           << " retry_delay_ms=" << progress->scheduled_retry_delay.count();
    if (progress->last_completed_attempt.has_value()) {
        const auto& completed = *progress->last_completed_attempt;
        stream << " completed_attempt=" << completed.id.value
               << " completed_publication=" << completed.publication.value
               << " native_status=" << dualsense_usb_status_name(completed.result.status)
               << " native_code=" << completed.result.native_code;
    } else {
        stream << " native_status=not_completed";
    }
    return stream.str();
}

} // namespace inputnext::gameinput_detail
