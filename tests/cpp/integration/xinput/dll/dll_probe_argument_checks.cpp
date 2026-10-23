#include "dll_probe_argument_checks.hpp"

#include "dll_probe_support.hpp"
#include "table_fixture_protocol.hpp"
#include <inputnext/xinput_types.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace inputnext::xinput_dll_probe {
namespace {

template <typename Value>
void initialize_canary(Value& value, const unsigned char byte) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::array<unsigned char, sizeof(Value)> bytes{};
    bytes.fill(byte);
    value = std::bit_cast<Value>(bytes);
}

template <typename Value>
[[nodiscard]] std::array<unsigned char, sizeof(Value)> snapshot_output(
    const Value& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    std::array<unsigned char, sizeof(Value)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

template <typename Value>
void require_bad_argument_output(const DWORD result,
    const Value& actual,
    const std::array<unsigned char, sizeof(Value)>& before,
    const char* const operation) {
    require(result == ERROR_BAD_ARGUMENTS,
        std::string{operation} + " did not return ERROR_BAD_ARGUMENTS exactly");
    require(snapshot_output(actual) == before,
        std::string{operation} + " changed an output canary on invalid arguments");
}

void require_bad_arguments(const DWORD result, const char* const operation) {
    require(result == ERROR_BAD_ARGUMENTS,
        std::string{operation} + " did not return ERROR_BAD_ARGUMENTS exactly");
}

} // namespace

void check_guarded_calls(const HMODULE facade14, const HMODULE facade13) {
    const auto get_state =
        load_named_function<DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept>(facade14,
            "XInputGetState");
    XINPUT_STATE state{};
    initialize_canary(state, 0xA1U);
    const auto state_before = snapshot_output(state);
    require_bad_argument_output(get_state(XUSER_MAX_COUNT, &state),
        state,
        state_before,
        "XInputGetState invalid index");
    require_bad_arguments(get_state(0U, nullptr), "XInputGetState null output");

    const auto set_state =
        load_named_function<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*) noexcept>(facade14,
            "XInputSetState");
    XINPUT_VIBRATION vibration{};
    require_bad_arguments(set_state(XUSER_MAX_COUNT, &vibration), "XInputSetState invalid index");
    require_bad_arguments(set_state(0U, nullptr), "XInputSetState null input");

    const auto get_capabilities =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*) noexcept>(facade14,
            "XInputGetCapabilities");
    XINPUT_CAPABILITIES capabilities{};
    initialize_canary(capabilities, 0xA2U);
    const auto capabilities_before = snapshot_output(capabilities);
    require_bad_argument_output(
        get_capabilities(XUSER_MAX_COUNT, XINPUT_FLAG_GAMEPAD, &capabilities),
        capabilities,
        capabilities_before,
        "XInputGetCapabilities invalid index");
    require_bad_argument_output(get_capabilities(0U, 0xFFFF'FFFFU, &capabilities),
        capabilities,
        capabilities_before,
        "XInputGetCapabilities invalid flags");
    require_bad_arguments(get_capabilities(0U, 0U, nullptr), "XInputGetCapabilities null output");

    const auto get_battery =
        load_named_function<DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*) noexcept>(
            facade14,
            "XInputGetBatteryInformation");
    XINPUT_BATTERY_INFORMATION battery{};
    initialize_canary(battery, 0xA3U);
    const auto battery_before = snapshot_output(battery);
    require_bad_argument_output(get_battery(XUSER_MAX_COUNT, BATTERY_DEVTYPE_GAMEPAD, &battery),
        battery,
        battery_before,
        "XInputGetBatteryInformation invalid index");
    require_bad_argument_output(get_battery(0U, static_cast<BYTE>(0xFFU), &battery),
        battery,
        battery_before,
        "XInputGetBatteryInformation invalid device type");
    require_bad_arguments(get_battery(0U, BATTERY_DEVTYPE_GAMEPAD, nullptr),
        "XInputGetBatteryInformation null output");

    const auto get_keystroke =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept>(facade14,
            "XInputGetKeystroke");
    XINPUT_KEYSTROKE keystroke{};
    initialize_canary(keystroke, 0xA4U);
    const auto keystroke_before = snapshot_output(keystroke);
    require_bad_argument_output(get_keystroke(XUSER_MAX_COUNT, 0xFFFF'FFFFU, &keystroke),
        keystroke,
        keystroke_before,
        "XInputGetKeystroke invalid index");
    require_bad_arguments(get_keystroke(0xFFU, 0xFFFF'FFFFU, nullptr),
        "XInputGetKeystroke null output");
    const auto get_keystroke13 =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept>(facade13,
            "XInputGetKeystroke");
    require_bad_argument_output(get_keystroke13(XUSER_MAX_COUNT, 0xFFFF'FFFFU, &keystroke),
        keystroke,
        keystroke_before,
        "XInput 1.3 GetKeystroke invalid index");
    require_bad_arguments(get_keystroke13(0xFFU, 0xFFFF'FFFFU, nullptr),
        "XInput 1.3 GetKeystroke null output");

    const auto get_audio =
        load_named_function<DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*) noexcept>(facade14,
            "XInputGetAudioDeviceIds");
    std::array<wchar_t, 8U> render_id{};
    std::array<wchar_t, 8U> capture_id{};
    render_id.fill(L'R');
    capture_id.fill(L'C');
    const auto render_id_before = render_id;
    const auto capture_id_before = capture_id;
    UINT render_count = 0x1234U;
    UINT capture_count = 0x5678U;
    require_bad_arguments(get_audio(XUSER_MAX_COUNT,
                              render_id.data(),
                              &render_count,
                              capture_id.data(),
                              &capture_count),
        "XInputGetAudioDeviceIds invalid index");
    require(render_id == render_id_before && capture_id == capture_id_before &&
                render_count == 0x1234U && capture_count == 0x5678U,
        "XInputGetAudioDeviceIds changed output canaries on invalid index");

    const auto get_state_ex =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept>(facade14, 100U);
    require_bad_argument_output(get_state_ex(XUSER_MAX_COUNT, &state),
        state,
        state_before,
        "XInputGetStateEx invalid index");
    require_bad_arguments(get_state_ex(0U, nullptr), "XInputGetStateEx null output");

    const auto get_capabilities_ex = load_ordinal_function<DWORD(
        WINAPI*)(DWORD, DWORD, inputnext::xinput::capabilities_ex*) noexcept>(facade14, 108U);
    inputnext::xinput::capabilities_ex capabilities_ex{};
    initialize_canary(capabilities_ex, 0xA5U);
    const auto capabilities_ex_before = snapshot_output(capabilities_ex);
    require_bad_argument_output(
        get_capabilities_ex(XUSER_MAX_COUNT, XINPUT_FLAG_GAMEPAD, &capabilities_ex),
        capabilities_ex,
        capabilities_ex_before,
        "XInputGetCapabilitiesEx invalid index");
    require_bad_argument_output(get_capabilities_ex(0U, 0xFFFF'FFFFU, &capabilities_ex),
        capabilities_ex,
        capabilities_ex_before,
        "XInputGetCapabilitiesEx invalid flags");
    require_bad_arguments(get_capabilities_ex(0U, 0U, nullptr),
        "XInputGetCapabilitiesEx null output");

    const auto wait_for_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, DWORD, LPVOID) noexcept>(facade14, 101U);
    require_bad_arguments(wait_for_guide(XUSER_MAX_COUNT, 0xFFFF'FFFFU, nullptr),
        "XInputWaitForGuideButton invalid index");
    const auto cancel_guide = load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade14, 102U);
    require_bad_arguments(cancel_guide(XUSER_MAX_COUNT),
        "XInputCancelGuideButtonWait invalid index");
    const auto power_off = load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade14, 103U);
    require_bad_arguments(power_off(XUSER_MAX_COUNT), "XInputPowerOffController invalid index");

    const auto get_dsound =
        load_named_function<DWORD(WINAPI*)(DWORD, GUID*, GUID*) noexcept>(facade13,
            "XInputGetDSoundAudioDeviceGuids");
    GUID render_guid{};
    GUID capture_guid{};
    initialize_canary(render_guid, 0xA6U);
    initialize_canary(capture_guid, 0xA7U);
    const auto render_guid_before = snapshot_output(render_guid);
    const auto capture_guid_before = snapshot_output(capture_guid);
    require_bad_argument_output(get_dsound(XUSER_MAX_COUNT, &render_guid, &capture_guid),
        render_guid,
        render_guid_before,
        "XInput 1.3 GetDSoundAudioDeviceGuids invalid index");
    require(snapshot_output(capture_guid) == capture_guid_before,
        "XInput 1.3 GetDSoundAudioDeviceGuids changed its second output canary");
}

void check_permissive_calls(const HMODULE facade14, const snapshot_fn get_snapshot) {
    const auto get_keystroke =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept>(facade14,
            "XInputGetKeystroke");
    XINPUT_KEYSTROKE keystroke{};
    require(get_keystroke(0xFFU, 0U, &keystroke) == expected_result(callback_id::get_keystroke),
        "GetKeystroke rejected XUSER_INDEX_ANY");
    expect_last_callback(get_snapshot,
        callback_id::get_keystroke,
        1U,
        {0xFFU, 0U, reinterpret_cast<std::uintptr_t>(&keystroke)});
    require(get_keystroke(0U, 0xFFFF'FFFFU, &keystroke) ==
                expected_result(callback_id::get_keystroke),
        "GetKeystroke rejected a permissive reserved value");
    expect_last_callback(get_snapshot,
        callback_id::get_keystroke,
        1U,
        {0U, 0xFFFF'FFFFU, reinterpret_cast<std::uintptr_t>(&keystroke)});

    const auto wait_for_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, DWORD, LPVOID) noexcept>(facade14, 101U);
    require(wait_for_guide(0U, 0xFFFF'FFFFU, nullptr) ==
                expected_result(callback_id::wait_for_guide_button),
        "Guide wait rejected permissive flags or ignored null context");
    expect_last_callback(get_snapshot,
        callback_id::wait_for_guide_button,
        1U,
        {0U, 0xFFFF'FFFFU, 0U});

    const auto get_audio =
        load_named_function<DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*) noexcept>(facade14,
            "XInputGetAudioDeviceIds");
    require(get_audio(0U, nullptr, nullptr, nullptr, nullptr) ==
                expected_result(callback_id::get_audio_device_ids),
        "1.4 audio IDs rejected permissive null optional buffers");
    expect_last_callback(get_snapshot, callback_id::get_audio_device_ids, 1U, {0U, 0U, 0U, 0U, 0U});

    const auto enable = load_named_function<void(WINAPI*)(BOOL) noexcept>(facade14, "XInputEnable");
    enable(FALSE);
    expect_last_callback(get_snapshot, callback_id::enable, 1U, {0U});
}

void check_permissive_dsound_call(const HMODULE facade13, const snapshot_fn get_snapshot) {
    const auto get_dsound =
        load_named_function<DWORD(WINAPI*)(DWORD, GUID*, GUID*) noexcept>(facade13,
            "XInputGetDSoundAudioDeviceGuids");
    require(get_dsound(0U, nullptr, nullptr) ==
                expected_result(callback_id::get_dsound_audio_device_guids),
        "1.3 DirectSound audio family rejected permissive null output pointers");
    expect_last_callback(get_snapshot,
        callback_id::get_dsound_audio_device_guids,
        0U,
        {0U, 0U, 0U});
}

} // namespace inputnext::xinput_dll_probe
