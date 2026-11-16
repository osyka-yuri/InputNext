#include "enumeration.hpp"

#include "dinput/catalogue/devices.hpp"
#include "dinput/device_info.hpp"
#include "dinput/device_input.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "factory.hpp"

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace inputnext::dinput::detail {
namespace {

struct enumeration_capture final {
    std::vector<DIDEVICEINSTANCEW>* instances{};
    HRESULT failure{S_OK};
};

[[nodiscard]] BOOL CALLBACK capture_instance(LPCDIDEVICEINSTANCEW instance,
    LPVOID context) noexcept {
    auto& capture = *static_cast<enumeration_capture*>(context);
    if (instance == nullptr || capture.instances == nullptr) {
        capture.failure = DIERR_INVALIDPARAM;
        return DIENUM_STOP;
    }
    try {
        capture.instances->push_back(*instance);
        return DIENUM_CONTINUE;
    } catch (const std::bad_alloc&) {
        capture.failure = DIERR_OUTOFMEMORY;
    } catch (...) {
        capture.failure = DIERR_GENERIC;
    }
    return DIENUM_STOP;
}

template <typename Callback>
[[nodiscard]] HRESULT invoke_enum_callback(Callback callback,
    const DIDEVICEINSTANCEW& instance,
    LPVOID context,
    BOOL& result) noexcept {
    try {
        if constexpr (std::is_same_v<Callback, LPDIENUMDEVICESCALLBACKW>) {
            result = callback(&instance, context);
        } else {
            DIDEVICEINSTANCEA ansi{};
            const auto status = copy_instance_ansi(instance, ansi);
            if (FAILED(status)) {
                return status;
            }
            result = callback(&ansi, context);
        }
    } catch (...) {
        return DIERR_GENERIC;
    }
    return S_OK;
}

struct enumeration_filter final {
    DWORD device_type{};
    DWORD flags{};
};

[[nodiscard]] catalogue_projection normalized_projection(const host& root_host,
    HINSTANCE application,
    enumeration_filter filter,
    IDirectInput8W& system) {
    catalogue_projection result{};
    if (!is_game_controller_request(filter.device_type) || root_host.session == nullptr) {
        return result;
    }
    const auto shared_access = root_host.session(root_host.context);
    if (shared_access == nullptr) {
        return result;
    }
    auto access = shared_access->enter();
    if (access && FAILED(project_catalogue(std::move(access), system, result))) {
        return {};
    }
    if ((filter.flags & DIEDFL_FORCEFEEDBACK) != 0U) {
        const auto call = shared_access->enter();
        const native_input_factory factory{.create = root_host.create_system,
            .context =
                root_host.create_system == &create_system_input ? nullptr : root_host.context,
            .instance = application,
            .lifetime = root_host.system_lifetime};
        std::erase_if(result.devices, [&](const auto& device) {
            return !call || !feedback_available(call, device, factory);
        });
    }
    return result;
}

[[nodiscard]] std::vector<DIDEVICEINSTANCEW> merge_instances(
    const std::vector<DIDEVICEINSTANCEW>& native,
    const catalogue_projection& projected,
    DWORD device_type) {
    std::vector<DIDEVICEINSTANCEW> result;
    result.reserve(native.size() + projected.devices.size());
    for (const auto& instance : native) {
        if (std::ranges::none_of(projected.suppressed_system_guids, [&instance](const GUID& guid) {
                return IsEqualGUID(guid, instance.guidInstance) != 0;
            })) {
            result.push_back(instance);
        }
    }
    for (const auto& device : projected.devices) {
        if (instance_matches_type(device.instance, device_type)) {
            result.push_back(device.instance);
        }
    }
    return result;
}

template <typename Callback>
[[nodiscard]] HRESULT deliver_instances(Callback callback,
    const std::vector<DIDEVICEINSTANCEW>& instances,
    LPVOID context) noexcept {
    for (const auto& instance : instances) {
        BOOL keep_going{};
        const auto status = invoke_enum_callback(callback, instance, context, keep_going);
        if (FAILED(status)) {
            return status;
        }
        if (keep_going == DIENUM_STOP) {
            break;
        }
    }
    return DI_OK;
}

template <typename Callback>
[[nodiscard]] HRESULT forward_enumeration(IDirectInput8W& system,
    DWORD device_type,
    Callback callback,
    LPVOID context,
    DWORD flags) noexcept {
    struct forwarding final {
        Callback callback{};
        LPVOID context{};
        HRESULT status{DI_OK};
    } bridge{.callback = callback, .context = context};

    const auto status = system.EnumDevices(
        device_type,
        [](LPCDIDEVICEINSTANCEW instance, LPVOID context) noexcept -> BOOL {
            auto& bridge = *static_cast<forwarding*>(context);
            if (instance == nullptr) {
                bridge.status = DIERR_INVALIDPARAM;
                return DIENUM_STOP;
            }
            BOOL keep_going{};
            bridge.status =
                invoke_enum_callback(bridge.callback, *instance, bridge.context, keep_going);
            return FAILED(bridge.status) ? DIENUM_STOP : keep_going;
        },
        &bridge,
        flags);
    return FAILED(bridge.status) ? bridge.status : status;
}

template <typename Callback>
[[nodiscard]] HRESULT enumerate_devices_impl(const host& root_host,
    HINSTANCE application,
    IDirectInput8W& system,
    DWORD device_type,
    Callback callback,
    LPVOID context,
    DWORD flags) noexcept {
    if (root_host.session == nullptr) {
        return forward_enumeration(system, device_type, callback, context, flags);
    }
    try {
        std::vector<DIDEVICEINSTANCEW> native;
        enumeration_capture capture{.instances = &native};
        const auto status = system.EnumDevices(device_type, &capture_instance, &capture, flags);
        if (FAILED(capture.failure)) {
            return capture.failure;
        }
        if (FAILED(status)) {
            return status;
        }
        const auto projected = normalized_projection(root_host,
            application,
            {.device_type = device_type, .flags = flags},
            system);
        const auto merged = merge_instances(native, projected, device_type);
        return deliver_instances(callback, merged, context);
    } catch (const std::bad_alloc&) {
        return DIERR_OUTOFMEMORY;
    } catch (...) {
        return DIERR_GENERIC;
    }
}

} // namespace

HRESULT enumerate_devices(const host& root_host,
    HINSTANCE application,
    IDirectInput8W& system,
    DWORD device_type,
    LPDIENUMDEVICESCALLBACKA callback,
    LPVOID context,
    DWORD flags) noexcept {
    return enumerate_devices_impl(root_host,
        application,
        system,
        device_type,
        callback,
        context,
        flags);
}

HRESULT enumerate_devices(const host& root_host,
    HINSTANCE application,
    IDirectInput8W& system,
    DWORD device_type,
    LPDIENUMDEVICESCALLBACKW callback,
    LPVOID context,
    DWORD flags) noexcept {
    return enumerate_devices_impl(root_host,
        application,
        system,
        device_type,
        callback,
        context,
        flags);
}

} // namespace inputnext::dinput::detail
