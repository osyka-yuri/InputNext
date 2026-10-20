#pragma once

#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>

#include <cstddef>
#include <string>

namespace inputnext::gameinput_detail {

struct keystroke_drain_report final {
    DWORD user_index{};
    DWORD result{};
    std::size_t drained{};
    bool limit_reached{};
};

[[nodiscard]] std::string format_xinput_keystroke(const xinput::assignment& assignment,
    DWORD result,
    const XINPUT_KEYSTROKE* keystroke);
[[nodiscard]] std::string format_xinput_keystroke_drain(const keystroke_drain_report& report);
[[nodiscard]] std::string format_xinput_guide_begin(DWORD user_index,
    const xinput::guide_wait_result<xinput::guide_wait_ticket>& result);
[[nodiscard]] std::string format_xinput_guide_wait(xinput::guide_wait_ticket ticket,
    const xinput::guide_wait_result<void>& result);
[[nodiscard]] std::string format_xinput_guide_cancel(xinput::guide_wait_ticket ticket,
    const xinput::guide_wait_result<bool>& result);
[[nodiscard]] std::string format_xinput_guide_wait_start_failure(xinput::guide_wait_ticket ticket,
    bool ticket_released);
[[nodiscard]] std::string format_xinput_guide_wait_exception(xinput::guide_wait_ticket ticket,
    bool ticket_released);

} // namespace inputnext::gameinput_detail
