#pragma once

#include <inputnext/xinput_types.hpp>

#include <Windows.h>
#include <Xinput.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

using input_next_xinput_flavor = std::uint32_t;

inline constexpr input_next_xinput_flavor INPUT_NEXT_FLAVOR_1_3 = 0U;
inline constexpr input_next_xinput_flavor INPUT_NEXT_FLAVOR_1_4 = 1U;
inline constexpr input_next_xinput_flavor INPUT_NEXT_FLAVOR_9_1_0 = 2U;

// Table and callbacks live in the provider image for its process lifetime. The producer pins the
// image before creating retained state; provider-owned state and indirect callback dependencies
// remain alive for the same lifetime. Consumers call callbacks outside loader lock and retain a
// matching provider bundle. Consumers cannot validate arbitrary provider-specific data pointers.
struct input_next_runtime_api_v1 final {
    std::uint32_t struct_size;
    std::uint32_t abi_version;

    DWORD(WINAPI* get_state)(input_next_xinput_flavor flavor,
        DWORD user_index,
        XINPUT_STATE* state) noexcept;
    DWORD(WINAPI* set_state)(input_next_xinput_flavor flavor,
        DWORD user_index,
        XINPUT_VIBRATION* vibration) noexcept;
    DWORD(WINAPI* get_capabilities)(input_next_xinput_flavor flavor,
        DWORD user_index,
        DWORD flags,
        XINPUT_CAPABILITIES* capabilities) noexcept;
    void(WINAPI* enable)(input_next_xinput_flavor flavor, BOOL enabled) noexcept;
    DWORD(WINAPI* get_battery_information)(input_next_xinput_flavor flavor,
        DWORD user_index,
        BYTE device_type,
        XINPUT_BATTERY_INFORMATION* battery) noexcept;
    DWORD(WINAPI* get_keystroke)(input_next_xinput_flavor flavor,
        DWORD user_index,
        DWORD reserved,
        PXINPUT_KEYSTROKE keystroke) noexcept;
    DWORD(WINAPI* get_audio_device_ids)(input_next_xinput_flavor flavor,
        DWORD user_index,
        LPWSTR render_device_id,
        UINT* render_count,
        LPWSTR capture_device_id,
        UINT* capture_count) noexcept;
    DWORD(WINAPI* get_dsound_audio_device_guids)(input_next_xinput_flavor flavor,
        DWORD user_index,
        GUID* render_guid,
        GUID* capture_guid) noexcept;
    DWORD(WINAPI* get_state_ex)(input_next_xinput_flavor flavor,
        DWORD user_index,
        XINPUT_STATE* state) noexcept;
    DWORD(WINAPI* get_capabilities_ex)(input_next_xinput_flavor flavor,
        DWORD user_index,
        DWORD flags,
        inputnext::xinput::capabilities_ex* capabilities) noexcept;
    DWORD(WINAPI* wait_for_guide_button)(input_next_xinput_flavor flavor,
        DWORD user_index,
        DWORD flags,
        LPVOID context) noexcept;
    DWORD(WINAPI* cancel_guide_button_wait)(input_next_xinput_flavor flavor,
        DWORD user_index) noexcept;
    DWORD(WINAPI* power_off_controller)(input_next_xinput_flavor flavor, DWORD user_index) noexcept;
};

extern "C" {

// Exported as ordinal 1 by the runtime module.
const input_next_runtime_api_v1* __cdecl input_next_runtime_query_api(
    std::uint32_t requested_abi) noexcept;

} // extern "C"

using input_next_runtime_query_api_fn = const input_next_runtime_api_v1*(
    __cdecl*)(std::uint32_t requested_abi) noexcept;

[[nodiscard]] inline bool is_valid_input_next_runtime_api_v1(
    const input_next_runtime_api_v1* api) noexcept {
    return api != nullptr && api->struct_size == sizeof(input_next_runtime_api_v1) &&
           api->abi_version == 1U && api->get_state != nullptr && api->set_state != nullptr &&
           api->get_capabilities != nullptr && api->enable != nullptr &&
           api->get_battery_information != nullptr && api->get_keystroke != nullptr &&
           api->get_audio_device_ids != nullptr && api->get_dsound_audio_device_guids != nullptr &&
           api->get_state_ex != nullptr && api->get_capabilities_ex != nullptr &&
           api->wait_for_guide_button != nullptr && api->cancel_guide_button_wait != nullptr &&
           api->power_off_controller != nullptr;
}

static_assert(sizeof(input_next_xinput_flavor) == sizeof(std::uint32_t));
static_assert(INPUT_NEXT_FLAVOR_1_3 == 0U);
static_assert(INPUT_NEXT_FLAVOR_1_4 == 1U);
static_assert(INPUT_NEXT_FLAVOR_9_1_0 == 2U);
static_assert(sizeof(inputnext::xinput::capabilities_ex) == 28U);
static_assert(offsetof(inputnext::xinput::capabilities_ex, vendor_id) == 20U);
static_assert(offsetof(inputnext::xinput::capabilities_ex, unknown1) == 26U);

using input_next_runtime_get_state_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, XINPUT_STATE*) noexcept;
using input_next_runtime_set_state_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, XINPUT_VIBRATION*) noexcept;
using input_next_runtime_get_capabilities_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, DWORD, XINPUT_CAPABILITIES*) noexcept;
using input_next_runtime_enable_fn = void(WINAPI*)(input_next_xinput_flavor, BOOL) noexcept;
using input_next_runtime_get_battery_information_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, BYTE, XINPUT_BATTERY_INFORMATION*) noexcept;
using input_next_runtime_get_keystroke_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept;
using input_next_runtime_get_audio_device_ids_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, LPWSTR, UINT*, LPWSTR, UINT*) noexcept;
using input_next_runtime_get_dsound_audio_device_guids_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, GUID*, GUID*) noexcept;
using input_next_runtime_get_state_ex_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, XINPUT_STATE*) noexcept;
using input_next_runtime_get_capabilities_ex_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, DWORD, inputnext::xinput::capabilities_ex*) noexcept;
using input_next_runtime_wait_for_guide_button_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD, DWORD, LPVOID) noexcept;
using input_next_runtime_cancel_guide_button_wait_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD) noexcept;
using input_next_runtime_power_off_controller_fn = DWORD(
    WINAPI*)(input_next_xinput_flavor, DWORD) noexcept;

static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_state),
    input_next_runtime_get_state_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::set_state),
    input_next_runtime_set_state_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_capabilities),
    input_next_runtime_get_capabilities_fn>);
static_assert(
    std::is_same_v<decltype(input_next_runtime_api_v1::enable), input_next_runtime_enable_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_battery_information),
    input_next_runtime_get_battery_information_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_keystroke),
    input_next_runtime_get_keystroke_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_audio_device_ids),
    input_next_runtime_get_audio_device_ids_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_dsound_audio_device_guids),
    input_next_runtime_get_dsound_audio_device_guids_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_state_ex),
    input_next_runtime_get_state_ex_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::get_capabilities_ex),
    input_next_runtime_get_capabilities_ex_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::wait_for_guide_button),
    input_next_runtime_wait_for_guide_button_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::cancel_guide_button_wait),
    input_next_runtime_cancel_guide_button_wait_fn>);
static_assert(std::is_same_v<decltype(input_next_runtime_api_v1::power_off_controller),
    input_next_runtime_power_off_controller_fn>);

static_assert(offsetof(input_next_runtime_api_v1, get_state) ==
              (2U * sizeof(std::uint32_t)) + (0U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, set_state) ==
              (2U * sizeof(std::uint32_t)) + (1U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_capabilities) ==
              (2U * sizeof(std::uint32_t)) + (2U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, enable) ==
              (2U * sizeof(std::uint32_t)) + (3U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_battery_information) ==
              (2U * sizeof(std::uint32_t)) + (4U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_keystroke) ==
              (2U * sizeof(std::uint32_t)) + (5U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_audio_device_ids) ==
              (2U * sizeof(std::uint32_t)) + (6U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_dsound_audio_device_guids) ==
              (2U * sizeof(std::uint32_t)) + (7U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_state_ex) ==
              (2U * sizeof(std::uint32_t)) + (8U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, get_capabilities_ex) ==
              (2U * sizeof(std::uint32_t)) + (9U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, wait_for_guide_button) ==
              (2U * sizeof(std::uint32_t)) + (10U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, cancel_guide_button_wait) ==
              (2U * sizeof(std::uint32_t)) + (11U * sizeof(void*)));
static_assert(offsetof(input_next_runtime_api_v1, power_off_controller) ==
              (2U * sizeof(std::uint32_t)) + (12U * sizeof(void*)));

static_assert(
    sizeof(input_next_runtime_api_v1) == (2U * sizeof(std::uint32_t)) + (13U * sizeof(void*)));
static_assert(sizeof(input_next_runtime_api_v1) == (sizeof(void*) == 8U ? 112U : 60U));
