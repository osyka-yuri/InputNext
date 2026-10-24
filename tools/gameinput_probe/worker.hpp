#pragma once

#include <cstdint>
#include <iosfwd>
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
};

[[nodiscard]] int run_probe_worker(const probe_options& options, std::ostream& output);

} // namespace inputnext::gameinput_detail
