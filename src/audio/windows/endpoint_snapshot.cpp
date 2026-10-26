#include "endpoint_snapshot.hpp"

#include "audio/native_types.hpp"

#include <inputnext/device_metadata.hpp>

#include <Windows.h>
#include <propidlbase.h>
#include <propkeydef.h>

#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>

namespace inputnext::audio_detail::windows {
namespace {

using Microsoft::WRL::ComPtr;

static_assert(sizeof(GUID) == native_guid_bytes);
static_assert(sizeof(wchar_t) == sizeof(char16_t));

struct task_memory_string_deleter final {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};

struct property_value final {
    property_value() noexcept = default;
    property_value(const property_value&) = delete;
    property_value& operator=(const property_value&) = delete;
    property_value(property_value&&) = delete;
    property_value& operator=(property_value&&) = delete;

    ~property_value() noexcept { static_cast<void>(PropVariantClear(&value)); }

    PROPVARIANT value{};
};

[[nodiscard]] native_error native_failure(const native_operation operation,
    const HRESULT result) noexcept {
    return {.operation = operation, .native_code = static_cast<std::uint32_t>(result)};
}

[[nodiscard]] native_endpoint_state map_endpoint_state(const DWORD value) noexcept {
    switch (value) {
    case DEVICE_STATE_ACTIVE:
        return native_endpoint_state::active;
    case DEVICE_STATE_DISABLED:
        return native_endpoint_state::disabled;
    case DEVICE_STATE_NOTPRESENT:
        return native_endpoint_state::not_present;
    case DEVICE_STATE_UNPLUGGED:
        return native_endpoint_state::unplugged;
    default:
        return native_endpoint_state::unknown;
    }
}

[[nodiscard]] native_result<fixed_haptic_endpoint_id> copy_endpoint_id(
    const wchar_t* value) noexcept {
    if (value == nullptr) {
        return std::unexpected{native_failure(native_operation::get_endpoint_id, E_POINTER)};
    }
    std::array<char16_t, haptic_endpoint_content_code_units> copied{};
    std::size_t length = 0U;
    while (length < copied.size() && value[length] != L'\0') {
        copied[length] = static_cast<char16_t>(value[length]);
        ++length;
    }
    if (length == copied.size() && value[length] != L'\0') {
        return std::unexpected{native_error{.operation = native_operation::get_endpoint_id,
            .native_code = ERROR_INSUFFICIENT_BUFFER}};
    }
    const auto result = fixed_haptic_endpoint_id::create({copied.data(), length});
    if (!result.has_value()) {
        return std::unexpected{native_failure(native_operation::get_endpoint_id, E_INVALIDARG)};
    }
    return *result;
}

[[nodiscard]] native_result<fixed_haptic_endpoint_id> read_endpoint_id(IMMDevice& device) noexcept {
    LPWSTR raw_id = nullptr;
    const HRESULT result = device.GetId(&raw_id);
    const std::unique_ptr<wchar_t, task_memory_string_deleter> owned_id{raw_id};
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::get_endpoint_id, result)};
    }
    return copy_endpoint_id(owned_id.get());
}

[[nodiscard]] native_result<native_endpoint_flow> read_endpoint_flow(IMMDevice& device) noexcept {
    ComPtr<IMMEndpoint> endpoint;
    HRESULT result = device.QueryInterface(IID_PPV_ARGS(endpoint.GetAddressOf()));
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::get_endpoint_flow, result)};
    }
    if (endpoint.Get() == nullptr) {
        return std::unexpected{native_failure(native_operation::get_endpoint_flow, E_UNEXPECTED)};
    }
    EDataFlow flow = eAll;
    result = endpoint->GetDataFlow(&flow);
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::get_endpoint_flow, result)};
    }
    if (flow == eRender) {
        return native_endpoint_flow::render;
    }
    if (flow == eCapture) {
        return native_endpoint_flow::capture;
    }
    return native_endpoint_flow::unknown;
}

[[nodiscard]] native_result<native_guid> read_container_id(IMMDevice& device) noexcept {
    ComPtr<IPropertyStore> properties;
    HRESULT result = device.OpenPropertyStore(STGM_READ, properties.GetAddressOf());
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::open_property_store, result)};
    }
    if (properties.Get() == nullptr) {
        return std::unexpected{native_failure(native_operation::open_property_store, E_UNEXPECTED)};
    }
    property_value container;
    result = properties->GetValue(PKEY_Device_ContainerId, &container.value);
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::get_container_id, result)};
    }
    if (container.value.vt != VT_CLSID || container.value.puuid == nullptr) {
        return std::unexpected{native_failure(native_operation::get_container_id, E_UNEXPECTED)};
    }
    native_guid copied{};
    std::memcpy(copied.bytes.data(), container.value.puuid, sizeof(GUID));
    return copied;
}

} // namespace

native_result<native_endpoint_snapshot> inspect_endpoint(IMMDevice& device) noexcept {
    const auto id = read_endpoint_id(device);
    if (!id.has_value()) {
        return std::unexpected{id.error()};
    }
    DWORD state = 0U;
    const HRESULT result = device.GetState(&state);
    if (FAILED(result)) {
        return std::unexpected{native_failure(native_operation::get_endpoint_state, result)};
    }
    const auto flow = read_endpoint_flow(device);
    if (!flow.has_value()) {
        return std::unexpected{flow.error()};
    }
    const auto container = read_container_id(device);
    if (!container.has_value()) {
        return std::unexpected{container.error()};
    }
    return native_endpoint_snapshot{.id = *id,
        .state = map_endpoint_state(state),
        .flow = *flow,
        .container_id = *container};
}

} // namespace inputnext::audio_detail::windows
