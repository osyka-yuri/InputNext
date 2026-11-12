#include "dualsense_usb_touch_reading.hpp"
#include "gameinput/dualsense/usb_touch_report.hpp"

#include <GameInput.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

struct raw_report_releaser final {
    void operator()(gameinput::IGameInputRawDeviceReport* report) const noexcept {
        if (report != nullptr) {
            report->Release();
        }
    }
};

} // namespace

std::optional<dualsense_usb_touch_contacts> read_dualsense_usb_touch(
    gameinput::IGameInputReading& reading,
    const bool observed_click) noexcept {
    gameinput::IGameInputRawDeviceReport* raw_report{};
    const bool report_available = reading.GetRawReport(&raw_report);
    const std::unique_ptr<gameinput::IGameInputRawDeviceReport, raw_report_releaser> report{
        raw_report};
    if (!report_available || report == nullptr) {
        return std::nullopt;
    }

    gameinput::GameInputRawDeviceReportInfo info{};
    report->GetReportInfo(&info);
    if (info.kind != gameinput::GameInputRawInputReport ||
        info.id != dualsense_usb_input_report_id || info.size != dualsense_usb_input_report_size ||
        report->GetRawDataSize() != dualsense_usb_input_report_size) {
        return std::nullopt;
    }

    std::array<std::uint8_t, dualsense_usb_input_report_size> packet{};
    if (report->GetRawData(packet.size(), packet.data()) != packet.size()) {
        return std::nullopt;
    }
    return decode_dualsense_usb_touch(std::span<const std::uint8_t>{packet}, observed_click);
}

} // namespace inputnext::gameinput_detail
