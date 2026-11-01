#include "gameinput_probe/input/xinput/event_report.hpp"

#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>

#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] const char* guide_error_name(xinput::guide_wait_error error) noexcept {
    switch (error) {
    case xinput::guide_wait_error::invalid_argument:
        return "invalid_argument";
    case xinput::guide_wait_error::invalid_ticket:
        return "invalid_ticket";
    case xinput::guide_wait_error::not_supported:
        return "not_supported";
    case xinput::guide_wait_error::not_connected:
        return "not_connected";
    case xinput::guide_wait_error::ineligible:
        return "ineligible";
    case xinput::guide_wait_error::capacity_exceeded:
        return "capacity_exceeded";
    case xinput::guide_wait_error::identity_exhausted:
        return "identity_exhausted";
    case xinput::guide_wait_error::busy:
        return "busy";
    case xinput::guide_wait_error::cancelled:
        return "cancelled";
    case xinput::guide_wait_error::history_reset:
        return "history_reset";
    case xinput::guide_wait_error::closed:
        return "closed";
    }
    return "unknown";
}

void append_result(std::ostream& output, DWORD result) {
    const char* name = "error";
    if (result == ERROR_SUCCESS) {
        name = "success";
    } else if (result == ERROR_EMPTY) {
        name = "empty";
    } else if (result == ERROR_DEVICE_NOT_CONNECTED) {
        name = "device_not_connected";
    } else if (result == ERROR_NOT_SUPPORTED) {
        name = "not_supported";
    }
    output << name << " code=0x" << std::hex << std::setw(8) << std::setfill('0') << result
           << std::dec << std::setfill(' ');
}

} // namespace

std::string format_xinput_keystroke(const xinput::assignment& assignment,
    DWORD result,
    const XINPUT_KEYSTROKE* keystroke) {
    std::ostringstream output;
    output << "xinput_keystroke user=" << assignment.user_index << " assignment=" << assignment.id
           << " result=";
    append_result(output, result);
    if (result == ERROR_SUCCESS && keystroke != nullptr) {
        output << " virtual_key=0x" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(keystroke->VirtualKey) << " unicode=0x" << std::setw(4)
               << static_cast<unsigned int>(keystroke->Unicode) << " flags=0x" << std::setw(4)
               << static_cast<unsigned int>(keystroke->Flags) << std::dec << std::setfill(' ')
               << " event_user=" << static_cast<unsigned int>(keystroke->UserIndex);
    }
    return output.str();
}

std::string format_xinput_keystroke_drain(const keystroke_drain_report& report) {
    std::ostringstream output;
    output << "xinput_keystroke_drain user=" << report.user_index << " result=";
    append_result(output, report.result);
    output << " drained=" << report.drained << " limit_reached=" << std::boolalpha
           << report.limit_reached;
    return output.str();
}

std::string format_xinput_guide_begin(DWORD user_index,
    const xinput::guide_wait_result<xinput::guide_wait_ticket>& result) {
    std::ostringstream output;
    output << "xinput_guide_begin user=" << user_index;
    if (!result.has_value()) {
        output << " result=error error=" << guide_error_name(result.error());
        return output.str();
    }
    output << " result=admitted ticket=" << result->id;
    return output.str();
}

std::string format_xinput_guide_wait(xinput::guide_wait_ticket ticket,
    const xinput::guide_wait_result<void>& result) {
    std::ostringstream output;
    output << "xinput_guide_wait user=" << ticket.user_index << " ticket=" << ticket.id;
    if (!result.has_value()) {
        output << " result=error error=" << guide_error_name(result.error());
    } else {
        output << " result=completed";
    }
    return output.str();
}

std::string format_xinput_guide_cancel(xinput::guide_wait_ticket ticket,
    const xinput::guide_wait_result<bool>& result) {
    std::ostringstream output;
    output << "xinput_guide_cancel user=" << ticket.user_index << " ticket=" << ticket.id;
    if (!result.has_value()) {
        output << " result=error error=" << guide_error_name(result.error());
    } else {
        output << " result=" << std::boolalpha << *result;
    }
    return output.str();
}

std::string format_xinput_guide_wait_start_failure(xinput::guide_wait_ticket ticket,
    bool ticket_released) {
    std::ostringstream output;
    output << "xinput_guide_wait user=" << ticket.user_index << " ticket=" << ticket.id
           << " result=thread_start_failed ticket_released=" << std::boolalpha << ticket_released;
    return output.str();
}

std::string format_xinput_guide_wait_exception(xinput::guide_wait_ticket ticket,
    bool ticket_released) {
    std::ostringstream output;
    output << "xinput_guide_wait user=" << ticket.user_index << " ticket=" << ticket.id
           << " result=caller_failed ticket_released=" << std::boolalpha << ticket_released;
    return output.str();
}

} // namespace inputnext::gameinput_detail
