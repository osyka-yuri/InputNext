#pragma once

#include "usb_touch_report.hpp"

#include <GameInput.h>

#include <optional>

namespace inputnext::input_qualification::diagnostic {

// Read the USB report and verify its click bit against the same reading's button state.
[[nodiscard]] std::optional<dualsense_usb_touch_contacts>
read_dualsense_usb_touch(GameInput::v4::IGameInputReading& reading, bool observed_click) noexcept;

} // namespace inputnext::input_qualification::diagnostic
