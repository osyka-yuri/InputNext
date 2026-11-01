#pragma once

#include <inputnext/output.hpp>

#include <span>
#include <string_view>

namespace inputnext::gameinput_detail {

struct dualsense_usb_step final {
    std::string_view name;
    output::channel_values values{};
    std::string_view instruction;
};

[[nodiscard]] std::span<const dualsense_usb_step> dualsense_usb_steps() noexcept;
[[nodiscard]] output::channel_values dualsense_usb_neutral_values() noexcept;

} // namespace inputnext::gameinput_detail
