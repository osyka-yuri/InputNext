#include "os_port.hpp"

#include "metadata/policy.hpp"
#include <inputnext/device_metadata.hpp>

#include <initguid.h>

#include <Windows.h>
#include <cfgmgr32.h>
#include <propidlbase.h>
#include <propkeydef.h>

#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <setupapi.h>
#include <wdmguid.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::metadata_detail {
namespace {

using Microsoft::WRL::ComPtr;

static_assert(sizeof(GUID) == native_guid_bytes);
static_assert(sizeof(wchar_t) == sizeof(char16_t));

[[nodiscard]] native_guid copy_guid(const GUID& guid) noexcept {
    native_guid result{};
    std::memcpy(result.bytes.data(), &guid, sizeof(guid));
    return result;
}

[[nodiscard]] GUID to_guid(const native_guid& value) noexcept {
    GUID result{};
    std::memcpy(&result, value.bytes.data(), sizeof(result));
    return result;
}

[[nodiscard]] bool equal_guid(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

[[nodiscard]] std::uint32_t win32_code(const DWORD code) noexcept {
    return static_cast<std::uint32_t>(code);
}

[[nodiscard]] std::uint32_t config_code(const CONFIGRET code) noexcept {
    return static_cast<std::uint32_t>(code);
}

[[nodiscard]] std::uint32_t hresult_code(const HRESULT code) noexcept {
    return static_cast<std::uint32_t>(code);
}

[[nodiscard]] metadata_field<native_guid> read_guid_property(const DEVINST device,
    const DEVPROPKEY& key) noexcept {
    GUID value{};
    DEVPROPTYPE type{};
    ULONG byte_count = sizeof(value);
    const CONFIGRET result = CM_Get_DevNode_PropertyW(device,
        &key,
        &type,
        reinterpret_cast<PBYTE>(&value),
        &byte_count,
        0U);
    if (result != CR_SUCCESS) {
        return {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = config_code(result)};
    }
    if (type != DEVPROP_TYPE_GUID || byte_count != sizeof(value)) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    const auto copied = copy_guid(value);
    if (copied.is_zero()) {
        return {.availability = metadata_availability::not_present, .value = {}, .native_code = 0U};
    }
    return {.availability = metadata_availability::available, .value = copied, .native_code = 0U};
}

[[nodiscard]] metadata_field<std::u16string> read_enumerator_name(const DEVINST device) {
    std::array<wchar_t, 128U> buffer{};
    DEVPROPTYPE type{};
    auto byte_count = static_cast<ULONG>(sizeof(buffer));
    const CONFIGRET result = CM_Get_DevNode_PropertyW(device,
        &DEVPKEY_Device_EnumeratorName,
        &type,
        reinterpret_cast<PBYTE>(buffer.data()),
        &byte_count,
        0U);
    if (result != CR_SUCCESS) {
        return {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = config_code(result)};
    }
    if (type != DEVPROP_TYPE_STRING || byte_count < sizeof(wchar_t) ||
        byte_count > sizeof(buffer) || byte_count % sizeof(wchar_t) != 0U) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    const auto units = static_cast<std::size_t>(byte_count / sizeof(wchar_t));
    const auto* const utf16 = reinterpret_cast<const char16_t*>(buffer.data());
    return copy_bounded_utf16({.data = utf16, .code_units = units}, units - 1U);
}

[[nodiscard]] metadata_field<transport_metadata> transport_for_device(const DEVINST device) {
    const auto enumerator = read_enumerator_name(device);
    if (enumerator.has_value()) {
        const std::u16string_view name = enumerator.value;
        if (name == u"BTH" || name == u"BTHENUM" || name == u"BTHLE") {
            return {.availability = metadata_availability::available,
                .value = {.value = physical_transport::bluetooth,
                    .provenance = transport_provenance::native_enumerator},
                .native_code = enumerator.native_code};
        }
        if (name == u"USB" || name == u"XUSB") {
            return {.availability = metadata_availability::available,
                .value = {.value = physical_transport::usb,
                    .provenance = transport_provenance::native_enumerator},
                .native_code = enumerator.native_code};
        }
        if (name == u"PCI" || name == u"1394" || name == u"I2C" || name == u"SPI") {
            return {.availability = metadata_availability::available,
                .value = {.value = physical_transport::other,
                    .provenance = transport_provenance::native_enumerator},
                .native_code = enumerator.native_code};
        }
    }

    const auto bus = read_guid_property(device, DEVPKEY_Device_BusTypeGuid);
    if (bus.has_value()) {
        const GUID native_bus = to_guid(bus.value);
        if (equal_guid(native_bus, GUID_BUS_TYPE_USB)) {
            return {.availability = metadata_availability::available,
                .value = {.value = physical_transport::usb,
                    .provenance = transport_provenance::native_bus_type},
                .native_code = bus.native_code};
        }
        if (equal_guid(native_bus, GUID_BUS_TYPE_PCI) ||
            equal_guid(native_bus, GUID_BUS_TYPE_1394)) {
            return {.availability = metadata_availability::available,
                .value = {.value = physical_transport::other,
                    .provenance = transport_provenance::native_bus_type},
                .native_code = bus.native_code};
        }
    }
    return {.availability = metadata_availability::unavailable,
        .value = {.value = physical_transport::unknown,
            .provenance = transport_provenance::unavailable},
        .native_code = 0U};
}

[[nodiscard]] metadata_field<transport_metadata> resolve_transport(const DEVINST starting_device) {
    DEVINST current = starting_device;
    for (unsigned int depth = 0U; depth < 5U; ++depth) {
        const auto result = transport_for_device(current);
        if (result.has_value()) {
            return result;
        }
        if (depth == 4U) {
            break;
        }
        DEVINST parent{};
        if (CM_Get_Parent(&parent, current, 0U) != CR_SUCCESS) {
            break;
        }
        current = parent;
    }
    return {.availability = metadata_availability::unavailable,
        .value = {.value = physical_transport::unknown,
            .provenance = transport_provenance::unavailable},
        .native_code = 0U};
}

class device_info_set final {
public:
    device_info_set() noexcept : handle_(SetupDiCreateDeviceInfoList(nullptr, nullptr)) {}

    device_info_set(const device_info_set&) = delete;
    device_info_set& operator=(const device_info_set&) = delete;
    device_info_set(device_info_set&&) = delete;
    device_info_set& operator=(device_info_set&&) = delete;

    ~device_info_set() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(SetupDiDestroyDeviceInfoList(handle_));
        }
    }

    [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

private:
    HDEVINFO handle_;
};

struct com_scope final {
    com_scope() noexcept {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        usable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        uninitialize = SUCCEEDED(result);
        native_code = hresult_code(result);
    }

    com_scope(const com_scope&) = delete;
    com_scope& operator=(const com_scope&) = delete;
    com_scope(com_scope&&) = delete;
    com_scope& operator=(com_scope&&) = delete;

    ~com_scope() {
        if (uninitialize) {
            CoUninitialize();
        }
    }

    bool usable{};
    bool uninitialize{};
    std::uint32_t native_code{};
};

struct prop_variant_scope final {
    prop_variant_scope() noexcept { PropVariantInit(&value); }

    prop_variant_scope(const prop_variant_scope&) = delete;
    prop_variant_scope& operator=(const prop_variant_scope&) = delete;
    prop_variant_scope(prop_variant_scope&&) = delete;
    prop_variant_scope& operator=(prop_variant_scope&&) = delete;

    ~prop_variant_scope() { PropVariantClear(&value); }

    PROPVARIANT value{};
};

struct task_memory_string_deleter final {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};

[[nodiscard]] metadata_field<native_guid> read_direct_sound_guid(IPropertyStore& properties) {
    prop_variant_scope endpoint_guid;
    const HRESULT result = properties.GetValue(PKEY_AudioEndpoint_GUID, &endpoint_guid.value);
    if (FAILED(result) || endpoint_guid.value.vt != VT_LPWSTR ||
        endpoint_guid.value.pwszVal == nullptr) {
        return {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = hresult_code(result)};
    }

    const auto text = copy_bounded_utf16(
        {.data = reinterpret_cast<const char16_t*>(endpoint_guid.value.pwszVal), .code_units = 64U},
        63U);
    if (!text.has_value()) {
        return {.availability = text.availability, .value = {}, .native_code = text.native_code};
    }

    const std::wstring guid_text(text.value.begin(), text.value.end());
    GUID direct_sound{};
    const HRESULT parse_result = IIDFromString(guid_text.c_str(), &direct_sound);
    if (FAILED(parse_result)) {
        return {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = hresult_code(parse_result)};
    }

    const auto copied_guid = copy_guid(direct_sound);
    if (copied_guid.is_zero()) {
        return {.availability = metadata_availability::not_present, .value = {}, .native_code = 0U};
    }
    return {.availability = metadata_availability::available,
        .value = copied_guid,
        .native_code = 0U};
}

[[nodiscard]] bool read_audio_endpoint(IMMDevice& device,
    const native_guid& target_container,
    audio_endpoint_observation& observation) {
    ComPtr<IPropertyStore> properties;
    HRESULT result = device.OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(result) || properties.Get() == nullptr) {
        return false;
    }

    prop_variant_scope native_container;
    result = properties->GetValue(PKEY_Device_ContainerId, &native_container.value);
    if (FAILED(result) || native_container.value.vt != VT_CLSID ||
        native_container.value.puuid == nullptr) {
        return false;
    }

    const auto endpoint_container = copy_guid(*native_container.value.puuid);
    if (endpoint_container.is_zero()) {
        return false;
    }
    if (endpoint_container != target_container) {
        return true;
    }

    observation.container_id = {.availability = metadata_availability::available,
        .value = endpoint_container,
        .native_code = 0U};
    LPWSTR raw_id = nullptr;
    result = device.GetId(&raw_id);
    std::unique_ptr<wchar_t, task_memory_string_deleter> const owned_id(raw_id);
    if (FAILED(result) || !owned_id) {
        observation.endpoint_id = {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = hresult_code(result)};
        return true;
    }

    observation.endpoint_id =
        copy_bounded_utf16({.data = reinterpret_cast<const char16_t*>(owned_id.get()),
                               .code_units = headset_endpoint_buffer_code_units},
            headset_endpoint_content_code_units);
    observation.direct_sound_guid = read_direct_sound_guid(*properties.Get());
    return true;
}

class system_windows_metadata_os final : public windows_metadata_os {
public:
    [[nodiscard]] exact_pnp_observation inspect_exact_pnp_interface(
        const std::string_view pnp_path_utf8) override {
        exact_pnp_observation observation{};
        if (pnp_path_utf8.empty() || pnp_path_utf8.size() > max_pnp_path_utf8_bytes ||
            pnp_path_utf8.contains('\0') ||
            pnp_path_utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            observation.container_id = {.availability = metadata_availability::capacity_exceeded,
                .value = {},
                .native_code = 0U};
            observation.transport = {.availability = metadata_availability::capacity_exceeded,
                .value = {.value = physical_transport::unknown,
                    .provenance = transport_provenance::unavailable},
                .native_code = 0U};
            return observation;
        }
        const std::string terminated_path{pnp_path_utf8};
        const int input_length = static_cast<int>(terminated_path.size());
        const int wide_length = MultiByteToWideChar(CP_UTF8,
            MB_ERR_INVALID_CHARS,
            terminated_path.c_str(),
            input_length,
            nullptr,
            0);
        if (wide_length <= 0) {
            const auto code = win32_code(GetLastError());
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = code};
            return observation;
        }
        std::wstring wide_path(static_cast<std::size_t>(wide_length), L'\0');
        if (MultiByteToWideChar(CP_UTF8,
                MB_ERR_INVALID_CHARS,
                terminated_path.c_str(),
                input_length,
                wide_path.data(),
                wide_length) != wide_length) {
            const auto code = win32_code(GetLastError());
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = code};
            return observation;
        }

        const device_info_set information;
        if (information.get() == INVALID_HANDLE_VALUE) {
            const auto code = win32_code(GetLastError());
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = code};
            return observation;
        }
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (SetupDiOpenDeviceInterfaceW(information.get(),
                wide_path.c_str(),
                0U,
                &interface_data) == FALSE) {
            const auto code = win32_code(GetLastError());
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = code};
            return observation;
        }

        SP_DEVINFO_DATA device_data{};
        device_data.cbSize = sizeof(device_data);
        DWORD required_bytes{};
        static_cast<void>(SetupDiGetDeviceInterfaceDetailW(information.get(),
            &interface_data,
            nullptr,
            0U,
            &required_bytes,
            nullptr));
        constexpr auto detail_limit = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) +
                                      ((max_pnp_path_utf8_bytes + 1U) * sizeof(wchar_t));
        if (required_bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = win32_code(GetLastError())};
            return observation;
        }
        if (required_bytes > detail_limit) {
            observation.container_id = {.availability = metadata_availability::capacity_exceeded,
                .value = {},
                .native_code = 0U};
            observation.transport = {.availability = metadata_availability::capacity_exceeded,
                .value = {.value = physical_transport::unknown,
                    .provenance = transport_provenance::unavailable},
                .native_code = 0U};
            return observation;
        }
        std::vector<std::byte> detail_storage(required_bytes);
        auto* const detail =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(information.get(),
                &interface_data,
                detail,
                required_bytes,
                nullptr,
                &device_data) == FALSE) {
            observation.container_id = {.availability = metadata_availability::unavailable,
                .value = {},
                .native_code = win32_code(GetLastError())};
            return observation;
        }

        observation.container_id =
            read_guid_property(device_data.DevInst, DEVPKEY_Device_ContainerId);
        observation.transport = resolve_transport(device_data.DevInst);
        return observation;
    }

    [[nodiscard]] audio_scan_result scan_active_audio(const audio_flow flow,
        const native_guid& container_id,
        const audio_endpoint_visitor visitor,
        void* const visitor_context) override {
        if (visitor == nullptr || container_id.is_zero()) {
            return {.availability = metadata_availability::unavailable, .native_code = 0U};
        }
        const com_scope apartment;
        if (!apartment.usable) {
            return {.availability = metadata_availability::unavailable,
                .native_code = apartment.native_code};
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result) || enumerator.Get() == nullptr) {
            return {.availability = metadata_availability::unavailable,
                .native_code = hresult_code(result)};
        }
        ComPtr<IMMDeviceCollection> collection;
        const EDataFlow native_flow = flow == audio_flow::render ? eRender : eCapture;
        result = enumerator->EnumAudioEndpoints(native_flow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(result) || collection.Get() == nullptr) {
            return {.availability = metadata_availability::unavailable,
                .native_code = hresult_code(result)};
        }
        UINT count{};
        result = collection->GetCount(&count);
        if (FAILED(result)) {
            return {.availability = metadata_availability::unavailable,
                .native_code = hresult_code(result)};
        }
        if (count > max_audio_endpoints_per_flow) {
            return {.availability = metadata_availability::capacity_exceeded, .native_code = 0U};
        }

        bool complete = true;
        bool capacity_exceeded = false;
        for (UINT index = 0U; index < count; ++index) {
            ComPtr<IMMDevice> device;
            result = collection->Item(index, &device);
            if (FAILED(result) || device.Get() == nullptr) {
                complete = false;
                continue;
            }
            audio_endpoint_observation observation{};
            if (!read_audio_endpoint(*device.Get(), container_id, observation)) {
                complete = false;
                continue;
            }
            if (!observation.container_id.has_value()) {
                continue;
            }
            if (observation.endpoint_id.availability == metadata_availability::capacity_exceeded) {
                capacity_exceeded = true;
            }
            visitor(visitor_context, observation);
        }
        if (capacity_exceeded) {
            return {.availability = metadata_availability::capacity_exceeded, .native_code = 0U};
        }
        return {.availability = complete ? metadata_availability::available
                                         : metadata_availability::unavailable,
            .native_code = 0U};
    }
};

} // namespace

std::unique_ptr<windows_metadata_os> make_system_windows_metadata_os() {
    return std::make_unique<system_windows_metadata_os>();
}

} // namespace inputnext::metadata_detail
