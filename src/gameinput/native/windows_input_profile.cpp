#include "windows_input_profile.hpp"

#include "gameinput/input_profile.hpp"
#include "gameinput/input_qualification.hpp"
#include "metadata/windows/os_port.hpp"
#include <inputnext/device_metadata.hpp>

#include <GameInput.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <winver.h>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

template <typename T>
[[nodiscard]] metadata_field<T> available(T value) noexcept {
    return {.availability = metadata_availability::available, .value = value};
}

[[nodiscard]] input_version copy_version(const gameinput::GameInputVersion& version) noexcept {
    return {.major = version.major,
        .minor = version.minor,
        .build = version.build,
        .revision = version.revision};
}

[[nodiscard]] std::optional<input_report_shape> read_single_input_report(
    gameinput::IGameInputDevice& device) noexcept {
    gameinput::GameInputRawDeviceReportInfo report{};
    if (device.GetInputReportCount() != 1U || device.GetInputReportInfo(1U, &report) != 1U ||
        report.kind != gameinput::GameInputRawInputReport) {
        return std::nullopt;
    }
    return input_report_shape{.id = report.id, .size = report.size};
}

[[nodiscard]] input_qualification_facts device_facts(gameinput::IGameInputDevice& device,
    const metadata_field<input_version>& runtime_version) noexcept {
    gameinput::GameInputVersion hardware{};
    gameinput::GameInputVersion firmware{};
    device.GetHardwareVersion(&hardware);
    device.GetFirmwareVersion(&firmware);
    const auto supported = device.GetSupportedInput();
    return {.runtime_version = runtime_version,
        .hardware_version = available(copy_version(hardware)),
        .firmware_version = available(copy_version(firmware)),
        .vendor_id = available(device.GetVendorId()),
        .product_id = available(device.GetProductId()),
        .revision = available(device.GetRevisionNumber()),
        .supported_input_bits = static_cast<std::uint32_t>(supported),
        .gamepad_supported = (supported & gameinput::GameInputKindGamepad) != 0U,
        .controller_button_supported = (supported & gameinput::GameInputKindControllerButton) != 0U,
        .button_count = device.GetControllerButtonCount(),
        .button_labels = {},
        .touch_surface_count = device.GetTouchSurfaceCount(),
        .single_input_report = (supported & gameinput::GameInputKindRawDeviceReport) != 0U
                                   ? read_single_input_report(device)
                                   : std::nullopt};
}

} // namespace

metadata_field<input_version> loaded_input_runtime_version(HMODULE module) {
    std::vector<wchar_t> path(32768U);
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        return {};
    }
    DWORD ignored{};
    const auto bytes = GetFileVersionInfoSizeW(path.data(), &ignored);
    constexpr DWORD maximum_version_bytes = 1024U * 1024U;
    if (bytes == 0U || bytes > maximum_version_bytes) {
        return {};
    }
    std::vector<std::byte> buffer(bytes);
    if (GetFileVersionInfoW(path.data(), 0U, bytes, buffer.data()) == FALSE) {
        return {};
    }
    VS_FIXEDFILEINFO* version{};
    UINT version_size{};
    if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&version), &version_size) ==
            FALSE ||
        version == nullptr || version_size < sizeof(VS_FIXEDFILEINFO) ||
        version->dwSignature != 0xFEEF04BDU) {
        return {};
    }
    return available(input_version{.major = HIWORD(version->dwFileVersionMS),
        .minor = LOWORD(version->dwFileVersionMS),
        .build = HIWORD(version->dwFileVersionLS),
        .revision = LOWORD(version->dwFileVersionLS)});
}

native_input_profile describe_windows_input(gameinput::IGameInputDevice& device,
    const metadata_field<input_version>& runtime_version,
    std::string_view pnp_path) {
    constexpr std::size_t button_capacity = 64U;
    auto facts = device_facts(device, runtime_version);
    if (facts.button_count == 0U || facts.button_count > button_capacity) {
        return qualify_native_input(facts);
    }
    std::array<gameinput::GameInputLabel, button_capacity> native_labels{};
    const auto copied = device.GetControllerButtonLabels(facts.button_count, native_labels.data());
    std::array<std::uint32_t, button_capacity> labels{};
    const auto copied_count = std::min<std::size_t>(copied, labels.size());
    for (std::size_t index = 0U; index < copied_count; ++index) {
        labels[index] = static_cast<std::uint32_t>(native_labels[index]);
    }
    facts.button_labels_complete = copied == facts.button_count;
    facts.button_labels = std::span{labels.data(), copied_count};
    if (!pnp_path.empty()) {
        const auto metadata = metadata_detail::make_system_windows_metadata_os();
        facts.transport = metadata->inspect_exact_pnp_interface(pnp_path).transport;
    }
    return qualify_native_input(facts);
}

} // namespace inputnext::gameinput_detail
