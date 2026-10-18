#include "report.hpp"

#include <array>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include <inputnext/device_metadata.hpp>
#include <inputnext/error.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/input_binding.hpp>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] const char* health_name(connection_health health) noexcept {
    switch (health) {
    case connection_health::discovering:
        return "discovering";
    case connection_health::callback_driven:
        return "callback_driven";
    case connection_health::polling_fallback:
        return "polling_fallback";
    case connection_health::retirement_pending:
        return "retirement_pending";
    case connection_health::failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] const char* registry_error_name(error value) noexcept {
    switch (value) {
    case error::no_data:
        return "no_data";
    case error::not_connected:
        return "not_connected";
    case error::invalid_argument:
        return "invalid_argument";
    case error::capacity_exceeded:
        return "capacity_exceeded";
    case error::identity_exhausted:
        return "identity_exhausted";
    case error::revision_exhausted:
        return "revision_exhausted";
    case error::closed:
        return "closed";
    }
    return "unknown";
}

[[nodiscard]] const char* runtime_error_name(gameinput_error value) noexcept {
    switch (value) {
    case gameinput_error::invalid_argument:
        return "invalid_argument";
    case gameinput_error::already_started:
        return "already_started";
    case gameinput_error::closed:
        return "closed";
    case gameinput_error::load_failed:
        return "load_failed";
    case gameinput_error::api_unavailable:
        return "api_unavailable";
    case gameinput_error::registration_failed:
        return "registration_failed";
    case gameinput_error::resource_exhausted:
        return "resource_exhausted";
    case gameinput_error::native_failure:
        return "native_failure";
    }
    return "unknown";
}

[[nodiscard]] const char* availability_name(metadata_availability availability) noexcept {
    switch (availability) {
    case metadata_availability::available:
        return "available";
    case metadata_availability::not_present:
        return "not_present";
    case metadata_availability::unsupported:
        return "unsupported";
    case metadata_availability::unavailable:
        return "unavailable";
    case metadata_availability::ambiguous:
        return "ambiguous";
    case metadata_availability::capacity_exceeded:
        return "capacity_exceeded";
    }
    return "unknown";
}

[[nodiscard]] const char* model_name(metadata_device_model model) noexcept {
    switch (model) {
    case metadata_device_model::generic_gamepad:
        return "generic_gamepad";
    case metadata_device_model::dualsense:
        return "dualsense";
    case metadata_device_model::dualsense_edge:
        return "dualsense_edge";
    }
    return "unknown";
}

[[nodiscard]] const char* transport_name(physical_transport transport) noexcept {
    switch (transport) {
    case physical_transport::usb:
        return "usb";
    case physical_transport::bluetooth:
        return "bluetooth";
    case physical_transport::other_wireless:
        return "other_wireless";
    case physical_transport::other:
        return "other";
    case physical_transport::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] const char* transport_source_name(transport_provenance source) noexcept {
    switch (source) {
    case transport_provenance::native_bus_type:
        return "native_bus_type";
    case transport_provenance::native_enumerator:
        return "native_enumerator";
    case transport_provenance::pnp_path_heuristic:
        return "pnp_path_heuristic";
    case transport_provenance::unavailable:
        return "unavailable";
    }
    return "unknown";
}

[[nodiscard]] const char* container_source_name(container_id_provenance source) noexcept {
    switch (source) {
    case container_id_provenance::gameinput_native:
        return "gameinput_native";
    case container_id_provenance::exact_pnp_interface:
        return "exact_pnp_interface";
    case container_id_provenance::conflicting:
        return "conflicting";
    case container_id_provenance::unavailable:
        return "unavailable";
    }
    return "unknown";
}

[[nodiscard]] const char* metadata_error_name(metadata_error value) noexcept {
    switch (value) {
    case metadata_error::closed:
        return "closed";
    case metadata_error::not_connected:
        return "not_connected";
    case metadata_error::busy:
        return "busy";
    case metadata_error::stale:
        return "stale";
    case metadata_error::resource_exhausted:
        return "resource_exhausted";
    }
    return "unknown";
}

[[nodiscard]] const char* boolean_name(bool value) noexcept {
    return value ? "true" : "false";
}

void append_guid_byte(std::ostream& output, const native_guid& value, std::size_t index) {
    output << std::hex << std::setw(2) << std::setfill('0')
           << std::to_integer<unsigned int>(value.bytes[index]);
}

void append_guid(std::ostream& output, const native_guid& value) {
    append_guid_byte(output, value, 3U);
    append_guid_byte(output, value, 2U);
    append_guid_byte(output, value, 1U);
    append_guid_byte(output, value, 0U);
    output << '-';
    append_guid_byte(output, value, 5U);
    append_guid_byte(output, value, 4U);
    output << '-';
    append_guid_byte(output, value, 7U);
    append_guid_byte(output, value, 6U);
    output << '-';
    append_guid_byte(output, value, 8U);
    append_guid_byte(output, value, 9U);
    output << '-';
    for (std::size_t index = 10U; index < value.bytes.size(); ++index) {
        append_guid_byte(output, value, index);
    }
    output << std::dec << std::setfill(' ');
}

void append_utf16_string(std::ostream& output, std::u16string_view value) {
    output << "u16\"";
    for (const char16_t code_unit : value) {
        const auto scalar = static_cast<unsigned int>(code_unit);
        if (scalar >= 0x20U && scalar <= 0x7EU && code_unit != u'\\' && code_unit != u'"') {
            output << static_cast<char>(code_unit);
        } else if (code_unit == u'\\' || code_unit == u'"') {
            output << '\\' << static_cast<char>(code_unit);
        } else {
            output << "\\u" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
                   << scalar << std::nouppercase << std::dec << std::setfill(' ');
        }
    }
    output << '"';
}

void append_endpoint(std::ostream& output,
    std::string_view name,
    const metadata_field<std::u16string>& field,
    std::size_t max_content_code_units) {
    output << ' ' << name << '=' << availability_name(field.availability);
    if (!field.has_value()) {
        return;
    }
    output << ' ' << name << "_id=";
    if (field.value.size() > max_content_code_units) {
        output << "over_limit:" << field.value.size() << "_code_units";
        return;
    }
    append_utf16_string(output, field.value);
}

void append_guid_field(std::ostream& output,
    std::string_view name,
    const metadata_field<native_guid>& field) {
    output << ' ' << name << '=' << availability_name(field.availability);
    if (field.has_value()) {
        output << ' ' << name << "_guid=";
        append_guid(output, field.value);
    }
}

} // namespace

std::string format_device(const gameinput_connection& connection,
    const result<connection_snapshot>& sample) {
    std::ostringstream output;
    output << "device key=" << connection.target.device.value
           << " connection=" << connection.target.connection.value
           << " health=" << health_name(connection.health)
           << " pad_known=" << boolean_name(connection.pad_known)
           << " guide_known=" << boolean_name(connection.guide_known);

    if (sample.has_value()) {
        const auto& value = sample->sample;
        output << " revision=" << value.revision << " source_us=" << value.source_timestamp_us
               << " observation_qpc=" << value.observation_qpc << " buttons=0x" << std::hex
               << value.state.buttons << std::dec << " left_stick=" << value.state.left_stick.x
               << ',' << value.state.left_stick.y << " right_stick=" << value.state.right_stick.x
               << ',' << value.state.right_stick.y << " triggers=" << value.state.left_trigger
               << ',' << value.state.right_trigger;
    } else {
        output << " sample_error=" << registry_error_name(sample.error());
    }
    return output.str();
}

std::string format_metadata(const metadata_snapshot& snapshot) {
    std::ostringstream output;
    output << "metadata key=" << snapshot.target.device.value
           << " vendor=" << availability_name(snapshot.native.vendor_id.availability);
    if (snapshot.native.vendor_id.has_value()) {
        output << ":0x" << std::hex << snapshot.native.vendor_id.value << std::dec;
    }
    output << " product=" << availability_name(snapshot.native.product_id.availability);
    if (snapshot.native.product_id.has_value()) {
        output << ":0x" << std::hex << snapshot.native.product_id.value << std::dec;
    }
    append_guid_field(output, "container", snapshot.container_id);
    output << " container_source=" << container_source_name(snapshot.container_id_source)
           << " model=" << availability_name(snapshot.model.availability);
    if (snapshot.model.has_value()) {
        output << ':' << model_name(snapshot.model.value);
    }
    output << " transport=" << availability_name(snapshot.transport.availability);
    if (snapshot.transport.has_value()) {
        output << ':' << transport_name(snapshot.transport.value.value) << " transport_source="
               << transport_source_name(snapshot.transport.value.provenance);
    }
    output << " battery=" << availability_name(snapshot.battery_telemetry.availability);
    append_endpoint(output,
        "headset_render",
        snapshot.headset_render.id,
        headset_endpoint_content_code_units);
    append_endpoint(output,
        "headset_capture",
        snapshot.headset_capture.id,
        headset_endpoint_content_code_units);
    append_endpoint(output,
        "haptic_endpoint",
        snapshot.native.haptic_endpoint_id,
        haptic_endpoint_content_code_units);
    output << " capabilities=" << availability_name(snapshot.capabilities.availability);
    if (snapshot.capabilities.has_value()) {
        output << " gamepad=" << boolean_name(snapshot.capabilities.value.gamepad)
               << " low_frequency_rumble="
               << boolean_name(snapshot.capabilities.value.low_frequency_rumble)
               << " high_frequency_rumble="
               << boolean_name(snapshot.capabilities.value.high_frequency_rumble)
               << " trigger_rumble=" << boolean_name(snapshot.capabilities.value.trigger_rumble);
    }
    return output.str();
}

std::string format_metadata_result(connection_target target,
    const metadata_result<metadata_snapshot>& metadata) {
    if (metadata.has_value()) {
        return format_metadata(metadata.value());
    }
    return "metadata key=" + std::to_string(target.device.value) +
           " query_error=" + metadata_error_name(metadata.error());
}

std::string format_start_failure(gameinput_failure failure) {
    std::ostringstream output;
    output << "start=failed code=" << runtime_error_name(failure.code) << " native_code=0x"
           << std::hex << failure.native_code;
    return output.str();
}

std::string format_stop_report(gameinput_stop_report report) {
    std::ostringstream output;
    output << "stop=" << (report.drained ? "drained" : "pending")
           << " callbacks_pending=" << report.pending_callback_owners
           << " metadata_queries=" << report.active_metadata_queries;
    return output.str();
}

requirement_report format_requirements(probe_requirements requirements) {
    const bool requirements_met = (!requirements.device_required || requirements.device_observed) &&
                                  (!requirements.reading_required || requirements.reading_observed);
    std::ostringstream output;
    output << "requirement=" << (requirements_met ? "met" : "not_met")
           << " device=" << boolean_name(requirements.device_observed)
           << " reading=" << boolean_name(requirements.reading_observed);
    return {.met = requirements_met, .line = output.str()};
}

} // namespace inputnext::gameinput_detail
