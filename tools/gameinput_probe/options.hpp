#pragma once

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

struct probe_options final {
    std::optional<std::uint32_t> duration_ms{10'000U};
    std::uint32_t first_step{1U};
    bool require_device{};
    bool require_reading{};
    bool history{};
    bool require_history{};
    bool xinput{};
    bool xinput_events{};
    bool rumble{};
    bool dualsense_usb{};
    bool dualsense_hd{};
};

} // namespace inputnext::gameinput_detail
