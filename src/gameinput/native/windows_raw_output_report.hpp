#pragma once

#include "native_raw_output_report.hpp"
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace GameInput::v4 {
struct IGameInputDevice;
struct IGameInputRawDeviceReport;
} // namespace GameInput::v4

namespace inputnext::gameinput_detail {

[[nodiscard]] output::native_result classify_raw_output_result(std::int32_t result) noexcept;

[[nodiscard]] bool fill_windows_raw_output_report(GameInput::v4::IGameInputRawDeviceReport& report,
    std::span<const std::byte> data) noexcept;

[[nodiscard]] raw_report_result
create_windows_raw_output_report(GameInput::v4::IGameInputDevice* device, std::uint32_t report_id);

} // namespace inputnext::gameinput_detail
