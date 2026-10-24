#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

enum class dualsense_usb_evidence : std::uint8_t {
    pending,
    accepted_newer_attempt,
    historical_reuse,
    rejected,
};

struct dualsense_usb_expectation final {
    output::channel_mask logical_channels{};
    output::channel_values logical_values{};
    output::native_output native_output{};
};

struct dualsense_usb_match final {
    output::backend_handle backend{};
    output::native_output native_output{};
    output::publication_id publication{};
    output::attempt_id attempt{};
    output::native_result result{};

    friend bool operator==(const dualsense_usb_match&, const dualsense_usb_match&) = default;
};

[[nodiscard]] dualsense_usb_evidence assess_dualsense_usb_evidence(
    const output::backend_handle& expected_backend,
    std::optional<output::attempt_id> before_attempt,
    const dualsense_usb_expectation& expected,
    const output::backend_progress& progress,
    const std::optional<output::logical_snapshot>& logical,
    const std::optional<dualsense_usb_match>& previous_match,
    dualsense_usb_match& matched) noexcept;

} // namespace inputnext::gameinput_detail
