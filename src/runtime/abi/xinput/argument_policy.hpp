#pragma once

#include "api.hpp"
#include <inputnext/xinput_types.hpp>

namespace inputnext::runtime_detail::xinput_argument_policy {

struct capabilities_arguments final {
    DWORD user_index;
    DWORD flags;
    XINPUT_CAPABILITIES* capabilities;
};

struct capabilities_ex_arguments final {
    DWORD user_index;
    DWORD flags;
    inputnext::xinput::capabilities_ex* capabilities;
};

struct battery_arguments final {
    DWORD user_index;
    BYTE device_type;
    XINPUT_BATTERY_INFORMATION* battery;
};

struct keystroke_arguments final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    PXINPUT_KEYSTROKE keystroke;
};

struct audio_device_ids_arguments final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
};

struct dsound_audio_device_guids_arguments final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
};

[[nodiscard]] constexpr DWORD validate_get_state_arguments(const DWORD user_index,
    const XINPUT_STATE* const state) noexcept {
    return user_index >= XUSER_MAX_COUNT || state == nullptr ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_set_state_arguments(const DWORD user_index,
    const XINPUT_VIBRATION* const vibration) noexcept {
    return user_index >= XUSER_MAX_COUNT || vibration == nullptr ? ERROR_BAD_ARGUMENTS
                                                                 : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_capabilities_arguments(
    const capabilities_arguments& arguments) noexcept {
    if (arguments.flags != 0U && arguments.flags != XINPUT_FLAG_GAMEPAD) {
        return ERROR_BAD_ARGUMENTS;
    }
    return arguments.user_index >= XUSER_MAX_COUNT || arguments.capabilities == nullptr
               ? ERROR_BAD_ARGUMENTS
               : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_capabilities_ex_arguments(
    const capabilities_ex_arguments& arguments) noexcept {
    if (arguments.flags != 0U && arguments.flags != XINPUT_FLAG_GAMEPAD) {
        return ERROR_BAD_ARGUMENTS;
    }
    return arguments.user_index >= XUSER_MAX_COUNT || arguments.capabilities == nullptr
               ? ERROR_BAD_ARGUMENTS
               : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_battery_arguments(
    const battery_arguments& arguments) noexcept {
    if (arguments.user_index >= XUSER_MAX_COUNT || arguments.battery == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }
    return arguments.device_type != BATTERY_DEVTYPE_GAMEPAD &&
                   arguments.device_type != BATTERY_DEVTYPE_HEADSET
               ? ERROR_BAD_ARGUMENTS
               : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_keystroke_arguments(
    const keystroke_arguments& arguments) noexcept {
    if (arguments.flavor == INPUT_NEXT_FLAVOR_9_1_0) {
        return ERROR_NOT_SUPPORTED;
    }
    constexpr DWORD any_user_index = 0xFFU;
    return arguments.keystroke == nullptr || (arguments.user_index >= XUSER_MAX_COUNT &&
                                                 arguments.user_index != any_user_index)
               ? ERROR_BAD_ARGUMENTS
               : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_audio_device_ids_arguments(
    const audio_device_ids_arguments& arguments) noexcept {
    if (arguments.flavor != INPUT_NEXT_FLAVOR_1_4) {
        return ERROR_NOT_SUPPORTED;
    }
    return arguments.user_index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_dsound_audio_device_guids_arguments(
    const dsound_audio_device_guids_arguments& arguments) noexcept {
    if (arguments.flavor == INPUT_NEXT_FLAVOR_1_4) {
        return ERROR_NOT_SUPPORTED;
    }
    return arguments.user_index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_guide_wait_arguments(const DWORD user_index) noexcept {
    return user_index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_cancel_guide_wait_arguments(
    const DWORD user_index) noexcept {
    return user_index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_power_off_arguments(const DWORD user_index) noexcept {
    return user_index >= XUSER_MAX_COUNT ? ERROR_BAD_ARGUMENTS : ERROR_SUCCESS;
}

[[nodiscard]] constexpr DWORD validate_enable_arguments(
    [[maybe_unused]] const BOOL enabled) noexcept {
    return ERROR_SUCCESS;
}

} // namespace inputnext::runtime_detail::xinput_argument_policy
