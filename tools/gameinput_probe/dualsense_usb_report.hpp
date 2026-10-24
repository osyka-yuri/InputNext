#pragma once

#include "dualsense_usb_evidence.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace inputnext::gameinput_detail {

struct dualsense_usb_step_report final {
    std::string_view step;
    connection_target target{};
    std::size_t step_number{};
    std::size_t step_count{};
    output::context_identity context{};
    std::uint64_t accepted_sequence{};
    std::uint64_t logical_version{};
    bool foreground{};
    dualsense_usb_evidence evidence{dualsense_usb_evidence::pending};
    std::optional<dualsense_usb_match> match;
};

[[nodiscard]] std::string format_dualsense_usb_step(const dualsense_usb_step_report& report);
[[nodiscard]] std::string format_dualsense_usb_progress(
    const std::optional<output::backend_progress>& progress);
[[nodiscard]] std::string_view dualsense_usb_evidence_name(
    dualsense_usb_evidence evidence) noexcept;
[[nodiscard]] std::string_view dualsense_usb_status_name(output::native_status status) noexcept;

} // namespace inputnext::gameinput_detail
