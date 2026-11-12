#pragma once

#include "gameinput/dualsense/usb_touch_report.hpp"

#include <GameInput.h>

#include <optional>

namespace inputnext::gameinput_detail {

// Read the complete raw input report from one retained reading and verify its click bit against
// the already-qualified controller-button state from that same reading.
[[nodiscard]] std::optional<dualsense_usb_touch_contacts>
read_dualsense_usb_touch(GameInput::v4::IGameInputReading& reading, bool observed_click) noexcept;

} // namespace inputnext::gameinput_detail
