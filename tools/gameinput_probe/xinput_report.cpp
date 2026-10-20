#include "xinput_report.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] const char* focus_evidence_name(focus::evidence value) noexcept {
    switch (value) {
    case focus::evidence::unavailable:
        return "unavailable";
    case focus::evidence::background:
        return "background";
    case focus::evidence::foreground:
        return "foreground";
    }
    return "unknown";
}

void append_runtime_id(std::ostream& output, const runtime_id& runtime) {
    for (const auto byte : runtime.bytes) {
        output << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned int>(byte);
    }
    output << std::dec << std::setfill(' ');
}

[[nodiscard]] bool neutral(const XINPUT_STATE& state) noexcept {
    const auto& gamepad = state.Gamepad;
    return gamepad.wButtons == 0U && gamepad.bLeftTrigger == 0U && gamepad.bRightTrigger == 0U &&
           gamepad.sThumbLX == 0 && gamepad.sThumbLY == 0 && gamepad.sThumbRX == 0 &&
           gamepad.sThumbRY == 0;
}

void append_state(std::ostream& output, const char* name, DWORD result, const XINPUT_STATE& state) {
    output << ' ' << name << "_result=0x" << std::hex << result << std::dec;
    if (result != ERROR_SUCCESS) {
        return;
    }
    output << ' ' << name << "_packet=" << state.dwPacketNumber << ' ' << name
           << "_neutral=" << std::boolalpha << neutral(state) << ' ' << name << "_buttons=0x"
           << std::hex << state.Gamepad.wButtons << std::dec << ' ' << name
           << "_triggers=" << static_cast<unsigned int>(state.Gamepad.bLeftTrigger) << ','
           << static_cast<unsigned int>(state.Gamepad.bRightTrigger) << ' ' << name
           << "_sticks=" << state.Gamepad.sThumbLX << ',' << state.Gamepad.sThumbLY << ','
           << state.Gamepad.sThumbRX << ',' << state.Gamepad.sThumbRY;
}

} // namespace

std::string format_xinput_focus(const focus::snapshot& snapshot) {
    std::ostringstream output;
    output << "xinput_focus state=" << focus_evidence_name(snapshot.state)
           << " revision=" << snapshot.revision << " observer_open=" << std::boolalpha
           << snapshot.open << " observations=" << snapshot.completed_observations
           << " process_pid=" << snapshot.process.pid
           << " process_creation_100ns=" << snapshot.process.creation_time_100ns
           << " native_code=0x" << std::hex << snapshot.native_code;
    return output.str();
}

std::string format_xinput_state(const xinput::assignment& assignment,
    std::optional<std::uint64_t> latest_revision,
    const focus::snapshot& focus_snapshot,
    DWORD standard_result,
    const XINPUT_STATE& standard,
    DWORD extended_result,
    const XINPUT_STATE& extended) {
    std::ostringstream output;
    output << "xinput_state user=" << assignment.user_index << " assignment=" << assignment.id
           << " runtime=";
    append_runtime_id(output, assignment.target.runtime);
    output << " key=" << assignment.target.device.value
           << " connection=" << assignment.target.connection.value
           << " focus=" << focus_evidence_name(focus_snapshot.state)
           << " focus_revision=" << focus_snapshot.revision << " latest_revision=";
    if (latest_revision.has_value()) {
        output << *latest_revision;
    } else {
        output << "unavailable";
    }
    append_state(output, "standard", standard_result, standard);
    append_state(output, "extended", extended_result, extended);
    return output.str();
}

std::string format_xinput_capabilities(const xinput::assignment& assignment,
    DWORD result,
    const xinput::capabilities_ex& capabilities) {
    std::ostringstream output;
    output << "xinput_capabilities user=" << assignment.user_index
           << " assignment=" << assignment.id << " key=" << assignment.target.device.value
           << " connection=" << assignment.target.connection.value << " result=0x" << std::hex
           << result << std::dec;
    if (result == ERROR_SUCCESS) {
        output << " type=" << static_cast<unsigned int>(capabilities.capabilities.Type)
               << " subtype=" << static_cast<unsigned int>(capabilities.capabilities.SubType)
               << " flags=0x" << std::hex << capabilities.capabilities.Flags << std::dec
               << " vendor=0x" << std::hex << capabilities.vendor_id << " product=0x"
               << capabilities.product_id << " version=0x" << capabilities.product_version
               << std::dec;
    }
    return output.str();
}

} // namespace inputnext::gameinput_detail
