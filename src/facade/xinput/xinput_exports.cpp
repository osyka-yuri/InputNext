#include "runtime_resolver.hpp"

#include "runtime/abi/xinput/api.hpp"
#include "runtime/abi/xinput/argument_policy.hpp"
#include <inputnext/xinput_types.hpp>

#include <Windows.h>
#include <Xinput.h>

#include <utility>

#ifndef INPUTNEXT_XINPUT_FLAVOR
#    error INPUTNEXT_XINPUT_FLAVOR must be set to 0, 1, or 2 for each facade target.
#endif

#if INPUTNEXT_XINPUT_FLAVOR < 0 || INPUTNEXT_XINPUT_FLAVOR > 2
#    error INPUTNEXT_XINPUT_FLAVOR must be 0, 1, or 2.
#endif

namespace {

constexpr auto facade_flavor = static_cast<input_next_xinput_flavor>(INPUTNEXT_XINPUT_FLAVOR);
using namespace inputnext::runtime_detail::xinput_argument_policy;

[[nodiscard]] const input_next_runtime_api_v1* api() noexcept {
    return inputnext::xinput_facade_detail::runtime_api();
}

template <typename Validator, typename Callback>
[[nodiscard]] DWORD guarded_call(Validator&& validate,
    const DWORD missing_runtime_status,
    Callback&& callback) noexcept {
    const auto argument_status = std::forward<Validator>(validate)();
    if (argument_status != ERROR_SUCCESS) {
        return argument_status;
    }
    const auto* const runtime = api();
    return runtime != nullptr ? std::forward<Callback>(callback)(*runtime) : missing_runtime_status;
}

} // namespace

extern "C" {

DWORD WINAPI InputNextXInputGetState(const DWORD user_index, XINPUT_STATE* const state) noexcept {
    return guarded_call([=] { return validate_get_state_arguments(user_index, state); },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_state(facade_flavor, user_index, state);
        });
}

DWORD WINAPI InputNextXInputSetState(const DWORD user_index,
    XINPUT_VIBRATION* const vibration) noexcept {
    return guarded_call([=] { return validate_set_state_arguments(user_index, vibration); },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.set_state(facade_flavor, user_index, vibration);
        });
}

DWORD WINAPI InputNextXInputGetCapabilities(const DWORD user_index,
    const DWORD flags,
    XINPUT_CAPABILITIES* const capabilities) noexcept {
    return guarded_call(
        [=] {
            return validate_capabilities_arguments(capabilities_arguments{.user_index = user_index,
                .flags = flags,
                .capabilities = capabilities});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_capabilities(facade_flavor, user_index, flags, capabilities);
        });
}

void WINAPI InputNextXInputEnable(const BOOL enabled) noexcept {
    if (validate_enable_arguments(enabled) != ERROR_SUCCESS) {
        return;
    }
    if (const auto* const runtime = api(); runtime != nullptr) {
        runtime->enable(facade_flavor, enabled);
    }
}

DWORD WINAPI InputNextXInputGetBatteryInformation(const DWORD user_index,
    const BYTE device_type,
    XINPUT_BATTERY_INFORMATION* const battery) noexcept {
    return guarded_call(
        [=] {
            return validate_battery_arguments(battery_arguments{.user_index = user_index,
                .device_type = device_type,
                .battery = battery});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_battery_information(facade_flavor, user_index, device_type, battery);
        });
}

DWORD WINAPI InputNextXInputGetKeystroke(const DWORD user_index,
    const DWORD reserved,
    PXINPUT_KEYSTROKE const keystroke) noexcept {
    return guarded_call(
        [=] {
            return validate_keystroke_arguments(keystroke_arguments{.flavor = facade_flavor,
                .user_index = user_index,
                .keystroke = keystroke});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_keystroke(facade_flavor, user_index, reserved, keystroke);
        });
}

DWORD WINAPI InputNextXInputGetAudioDeviceIds(const DWORD user_index,
    LPWSTR const render_device_id,
    UINT* const render_count,
    LPWSTR const capture_device_id,
    UINT* const capture_count) noexcept {
    return guarded_call(
        [=] {
            return validate_audio_device_ids_arguments(
                audio_device_ids_arguments{.flavor = facade_flavor, .user_index = user_index});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_audio_device_ids(facade_flavor,
                user_index,
                render_device_id,
                render_count,
                capture_device_id,
                capture_count);
        });
}

DWORD WINAPI InputNextXInputGetDSoundAudioDeviceGuids(const DWORD user_index,
    GUID* const render_guid,
    GUID* const capture_guid) noexcept {
    return guarded_call(
        [=] {
            return validate_dsound_audio_device_guids_arguments(
                dsound_audio_device_guids_arguments{.flavor = facade_flavor,
                    .user_index = user_index});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_dsound_audio_device_guids(facade_flavor,
                user_index,
                render_guid,
                capture_guid);
        });
}

DWORD WINAPI InputNextXInputGetStateEx(const DWORD user_index, XINPUT_STATE* const state) noexcept {
    return guarded_call([=] { return validate_get_state_arguments(user_index, state); },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_state_ex(facade_flavor, user_index, state);
        });
}

DWORD WINAPI InputNextXInputGetCapabilitiesEx(const DWORD user_index,
    const DWORD flags,
    inputnext::xinput::capabilities_ex* const capabilities) noexcept {
    return guarded_call(
        [=] {
            return validate_capabilities_ex_arguments(
                capabilities_ex_arguments{.user_index = user_index,
                    .flags = flags,
                    .capabilities = capabilities});
        },
        ERROR_DEVICE_NOT_CONNECTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.get_capabilities_ex(facade_flavor, user_index, flags, capabilities);
        });
}

DWORD WINAPI InputNextXInputWaitForGuideButton(const DWORD user_index,
    const DWORD flags,
    LPVOID const context) noexcept {
    return guarded_call([=] { return validate_guide_wait_arguments(user_index); },
        ERROR_NOT_SUPPORTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.wait_for_guide_button(facade_flavor, user_index, flags, context);
        });
}

DWORD WINAPI InputNextXInputCancelGuideButtonWait(const DWORD user_index) noexcept {
    return guarded_call([=] { return validate_cancel_guide_wait_arguments(user_index); },
        ERROR_NOT_SUPPORTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.cancel_guide_button_wait(facade_flavor, user_index);
        });
}

DWORD WINAPI InputNextXInputPowerOffController(const DWORD user_index) noexcept {
    return guarded_call([=] { return validate_power_off_arguments(user_index); },
        ERROR_NOT_SUPPORTED,
        [=](const input_next_runtime_api_v1& runtime) {
            return runtime.power_off_controller(facade_flavor, user_index);
        });
}

} // extern "C"
