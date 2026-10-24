#include "windows_raw_output_report.hpp"

#include "native_raw_output_report.hpp"
#include <inputnext/output_execution.hpp>

#include <expected>
#include <memory>

#include <Windows.h>

#include <GameInput.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

[[nodiscard]] constexpr std::int64_t native_code(const HRESULT result) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint32_t>(result));
}

class windows_raw_output_report final : public native_raw_output_report {
public:
    windows_raw_output_report(gameinput::IGameInputDevice* device,
        gameinput::IGameInputRawDeviceReport* report) noexcept
        : device_(device),
          report_(report) {}

    ~windows_raw_output_report() noexcept override {
        if (report_ != nullptr) {
            report_->Release();
        }
        if (device_ != nullptr) {
            device_->Release();
        }
    }

    windows_raw_output_report(const windows_raw_output_report&) = delete;
    windows_raw_output_report& operator=(const windows_raw_output_report&) = delete;
    windows_raw_output_report(windows_raw_output_report&&) = delete;
    windows_raw_output_report& operator=(windows_raw_output_report&&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept override {
        return report_ != nullptr ? report_->GetRawDataSize() : 0U;
    }

    [[nodiscard]] bool set_data(const std::span<const std::byte> data) noexcept override {
        return report_ != nullptr && fill_windows_raw_output_report(*report_, data);
    }

    [[nodiscard]] output::native_result send() noexcept override {
        if (device_ == nullptr || report_ == nullptr) {
            return classify_raw_output_result(E_POINTER);
        }
        return classify_raw_output_result(device_->SendRawDeviceOutput(report_));
    }

private:
    gameinput::IGameInputDevice* device_{};
    gameinput::IGameInputRawDeviceReport* report_{};
};

} // namespace

bool fill_windows_raw_output_report(gameinput::IGameInputRawDeviceReport& report,
    const std::span<const std::byte> data) noexcept {
    if (data.empty() || data.size() != report.GetRawDataSize()) {
        return false;
    }
    return report.SetRawData(data.size(), data.data()) && report.GetRawDataSize() == data.size();
}

output::native_result classify_raw_output_result(const std::int32_t native_result) noexcept {
    const HRESULT result = native_result;
    const auto code = native_code(result);
    if (SUCCEEDED(result)) {
        return output::native_result{.status = output::native_status::accepted,
            .native_code = code};
    }
    if (result == gameinput::GAMEINPUT_E_DEVICE_DISCONNECTED ||
        result == gameinput::GAMEINPUT_E_DEVICE_NOT_FOUND ||
        result == gameinput::GAMEINPUT_E_OBJECT_NO_LONGER_EXISTS) {
        return output::native_result{.status = output::native_status::disconnected,
            .native_code = code};
    }
    if (result == E_NOTIMPL || result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
        result == HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED) ||
        result == gameinput::GAMEINPUT_E_FEEDBACK_NOT_SUPPORTED ||
        result == gameinput::GAMEINPUT_E_AGGREGATE_OPERATION_NOT_SUPPORTED) {
        return output::native_result{.status = output::native_status::unsupported,
            .native_code = code};
    }
    if (result == E_INVALIDARG || result == E_POINTER || result == E_ACCESSDENIED) {
        return output::native_result{.status = output::native_status::failed, .native_code = code};
    }
    return output::native_result{.status = output::native_status::retryable_failure,
        .native_code = code};
}

raw_report_result create_windows_raw_output_report(gameinput::IGameInputDevice* device,
    const std::uint32_t report_id) {
    if (device == nullptr) {
        return std::unexpected(classify_raw_output_result(E_INVALIDARG));
    }

    gameinput::IGameInputRawDeviceReport* report{};
    const auto result =
        device->CreateRawDeviceReport(report_id, gameinput::GameInputRawOutputReport, &report);
    if (FAILED(result)) {
        if (report != nullptr) {
            report->Release();
        }
        return std::unexpected(classify_raw_output_result(result));
    }
    if (report == nullptr) {
        return std::unexpected(
            output::native_result{.status = output::native_status::retryable_failure});
    }

    device->AddRef();
    auto* const wrapped = new (std::nothrow) windows_raw_output_report{device, report};
    if (wrapped == nullptr) {
        report->Release();
        device->Release();
        return std::unexpected(classify_raw_output_result(E_OUTOFMEMORY));
    }
    return std::unique_ptr<native_raw_output_report>{wrapped};
}

} // namespace inputnext::gameinput_detail
