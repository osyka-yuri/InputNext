#include "table_fixture_protocol.hpp"

#include <inputnext/xinput_types.hpp>

#define INPUTNEXT_EXTERNAL_CALLBACK_FIXTURE_IMPORT 1
#include "external_callback_fixture_protocol.hpp"
#undef INPUTNEXT_EXTERNAL_CALLBACK_FIXTURE_IMPORT
#include "runtime/abi/xinput/api.hpp"

#include <array>
#include <cstdint>

namespace inputnext::xinput_dll_fixture {
namespace {

SRWLOCK state_lock = SRWLOCK_INIT;
snapshot fixture_state{};
#ifndef INPUTNEXT_XINPUT_FIXTURE_DEFAULT_QUERY_MODE
#    define INPUTNEXT_XINPUT_FIXTURE_DEFAULT_QUERY_MODE 0
#endif
constinit query_mode configured_mode =
    static_cast<query_mode>(INPUTNEXT_XINPUT_FIXTURE_DEFAULT_QUERY_MODE);
HMODULE reentry_target{};
thread_local bool inside_query{};

[[nodiscard]] DWORD record(const callback_id id,
    const input_next_xinput_flavor flavor,
    const std::array<std::uintptr_t, 6U>& arguments) noexcept {
    const DWORD result = expected_result(id);
    AcquireSRWLockExclusive(&state_lock);
    ++fixture_state.callback_calls;
    fixture_state.last_callback = static_cast<std::uint32_t>(id);
    fixture_state.last_flavor = static_cast<std::uint32_t>(flavor);
    fixture_state.last_arguments[0U] = arguments[0U];
    fixture_state.last_arguments[1U] = arguments[1U];
    fixture_state.last_arguments[2U] = arguments[2U];
    fixture_state.last_arguments[3U] = arguments[3U];
    fixture_state.last_arguments[4U] = arguments[4U];
    fixture_state.last_arguments[5U] = arguments[5U];
    fixture_state.last_callback_result = result;
    ReleaseSRWLockExclusive(&state_lock);
    return result;
}

[[nodiscard]] DWORD WINAPI get_state(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    XINPUT_STATE* const state) noexcept {
    return record(callback_id::get_state,
        flavor,
        {static_cast<std::uintptr_t>(user_index), reinterpret_cast<std::uintptr_t>(state)});
}

[[nodiscard]] DWORD WINAPI set_state(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    XINPUT_VIBRATION* const vibration) noexcept {
    return record(callback_id::set_state,
        flavor,
        {static_cast<std::uintptr_t>(user_index), reinterpret_cast<std::uintptr_t>(vibration)});
}

[[nodiscard]] DWORD WINAPI get_capabilities(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    const DWORD flags,
    XINPUT_CAPABILITIES* const capabilities) noexcept {
    return record(callback_id::get_capabilities,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            static_cast<std::uintptr_t>(flags),
            reinterpret_cast<std::uintptr_t>(capabilities)});
}

void WINAPI enable(const input_next_xinput_flavor flavor, const BOOL enabled) noexcept {
    static_cast<void>(record(callback_id::enable, flavor, {static_cast<std::uintptr_t>(enabled)}));
}

[[nodiscard]] DWORD WINAPI get_battery_information(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    const BYTE device_type,
    XINPUT_BATTERY_INFORMATION* const battery) noexcept {
    return record(callback_id::get_battery_information,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            static_cast<std::uintptr_t>(device_type),
            reinterpret_cast<std::uintptr_t>(battery)});
}

[[nodiscard]] DWORD WINAPI get_keystroke(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    const DWORD reserved,
    PXINPUT_KEYSTROKE const keystroke) noexcept {
    return record(callback_id::get_keystroke,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            static_cast<std::uintptr_t>(reserved),
            reinterpret_cast<std::uintptr_t>(keystroke)});
}

[[nodiscard]] DWORD WINAPI get_audio_device_ids(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    LPWSTR const render_device_id,
    UINT* const render_count,
    LPWSTR const capture_device_id,
    UINT* const capture_count) noexcept {
    return record(callback_id::get_audio_device_ids,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            reinterpret_cast<std::uintptr_t>(render_device_id),
            reinterpret_cast<std::uintptr_t>(render_count),
            reinterpret_cast<std::uintptr_t>(capture_device_id),
            reinterpret_cast<std::uintptr_t>(capture_count)});
}

[[nodiscard]] DWORD WINAPI get_dsound_audio_device_guids(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    GUID* const render_guid,
    GUID* const capture_guid) noexcept {
    return record(callback_id::get_dsound_audio_device_guids,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            reinterpret_cast<std::uintptr_t>(render_guid),
            reinterpret_cast<std::uintptr_t>(capture_guid)});
}

[[nodiscard]] DWORD WINAPI get_state_ex(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    XINPUT_STATE* const state) noexcept {
    return record(callback_id::get_state_ex,
        flavor,
        {static_cast<std::uintptr_t>(user_index), reinterpret_cast<std::uintptr_t>(state)});
}

[[nodiscard]] DWORD WINAPI get_capabilities_ex(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    const DWORD flags,
    inputnext::xinput::capabilities_ex* const capabilities) noexcept {
    return record(callback_id::get_capabilities_ex,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            static_cast<std::uintptr_t>(flags),
            reinterpret_cast<std::uintptr_t>(capabilities)});
}

[[nodiscard]] DWORD WINAPI wait_for_guide_button(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    const DWORD flags,
    LPVOID const context) noexcept {
    return record(callback_id::wait_for_guide_button,
        flavor,
        {static_cast<std::uintptr_t>(user_index),
            static_cast<std::uintptr_t>(flags),
            reinterpret_cast<std::uintptr_t>(context)});
}

[[nodiscard]] DWORD WINAPI cancel_guide_button_wait(const input_next_xinput_flavor flavor,
    const DWORD user_index) noexcept {
    return record(callback_id::cancel_guide_button_wait,
        flavor,
        {static_cast<std::uintptr_t>(user_index)});
}

[[nodiscard]] DWORD WINAPI power_off_controller(const input_next_xinput_flavor flavor,
    const DWORD user_index) noexcept {
    return record(callback_id::power_off_controller,
        flavor,
        {static_cast<std::uintptr_t>(user_index)});
}

constexpr input_next_runtime_api_v1 valid_api{.struct_size = sizeof(input_next_runtime_api_v1),
    .abi_version = 1U,
    .get_state = &get_state,
    .set_state = &set_state,
    .get_capabilities = &get_capabilities,
    .enable = &enable,
    .get_battery_information = &get_battery_information,
    .get_keystroke = &get_keystroke,
    .get_audio_device_ids = &get_audio_device_ids,
    .get_dsound_audio_device_guids = &get_dsound_audio_device_guids,
    .get_state_ex = &get_state_ex,
    .get_capabilities_ex = &get_capabilities_ex,
    .wait_for_guide_button = &wait_for_guide_button,
    .cancel_guide_button_wait = &cancel_guide_button_wait,
    .power_off_controller = &power_off_controller};

[[nodiscard]] constexpr input_next_runtime_api_v1 make_invalid_size_api() noexcept {
    auto api = valid_api;
    api.struct_size -= 4U;
    return api;
}

[[nodiscard]] constexpr input_next_runtime_api_v1 make_invalid_version_api() noexcept {
    auto api = valid_api;
    api.abi_version = 2U;
    return api;
}

[[nodiscard]] constexpr input_next_runtime_api_v1 make_missing_callback_api() noexcept {
    auto api = valid_api;
    api.cancel_guide_button_wait = nullptr;
    return api;
}

[[nodiscard]] constexpr input_next_runtime_api_v1 make_external_callback_api() noexcept {
    auto api = valid_api;
    api.get_state = &inputnext_external_get_state;
    return api;
}

constinit const input_next_runtime_api_v1 invalid_size_api = make_invalid_size_api();
constinit const input_next_runtime_api_v1 invalid_version_api = make_invalid_version_api();
constinit const input_next_runtime_api_v1 missing_callback_api = make_missing_callback_api();
const input_next_runtime_api_v1 external_callback_api = make_external_callback_api();

[[nodiscard]] const input_next_runtime_api_v1* configured_api(const query_mode mode) noexcept {
    switch (mode) {
    case query_mode::valid:
    case query_mode::reenter_same_facade:
    case query_mode::reenter_cross_facade:
        return &valid_api;
    case query_mode::invalid_size:
        return &invalid_size_api;
    case query_mode::invalid_version:
        return &invalid_version_api;
    case query_mode::missing_callback:
        return &missing_callback_api;
    case query_mode::null_table:
        return nullptr;
    case query_mode::external_table:
        return inputnext_external_get_api();
    case query_mode::external_callback:
        return &external_callback_api;
    }
    return nullptr;
}

void call_reentrant_facade(const HMODULE target) noexcept {
    if (target == nullptr) {
        return;
    }
    const FARPROC address = GetProcAddress(target, "XInputGetState");
    if (address == nullptr) {
        return;
    }
    using get_state_fn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept;
    const auto get_state_function = reinterpret_cast<get_state_fn>(address);
    if (get_state_function == nullptr) {
        return;
    }

    XINPUT_STATE state{};
    const DWORD result = get_state_function(0U, &state);
    AcquireSRWLockExclusive(&state_lock);
    ++fixture_state.reentry_calls;
    fixture_state.reentry_result = result;
    ReleaseSRWLockExclusive(&state_lock);
}

} // namespace
} // namespace inputnext::xinput_dll_fixture

extern "C" const input_next_runtime_api_v1* __cdecl input_next_runtime_query_api(
    const std::uint32_t requested_abi) noexcept {
    using namespace inputnext::xinput_dll_fixture;

    query_mode mode{};
    HMODULE target{};
    AcquireSRWLockExclusive(&state_lock);
    ++fixture_state.query_calls;
    fixture_state.last_requested_abi = requested_abi;
    mode = configured_mode;
    target = reentry_target;
    ReleaseSRWLockExclusive(&state_lock);

    if (inside_query || requested_abi != 1U) {
        return nullptr;
    }

    inside_query = true;
    if (mode == query_mode::reenter_same_facade || mode == query_mode::reenter_cross_facade) {
        call_reentrant_facade(target);
    }
    inside_query = false;
    return configured_api(mode);
}

extern "C" BOOL __cdecl inputnext_fixture_configure(const std::uint32_t mode,
    const HMODULE target) noexcept {
    using namespace inputnext::xinput_dll_fixture;
    if (mode > static_cast<std::uint32_t>(query_mode::external_callback)) {
        return FALSE;
    }
    const auto selected_mode = static_cast<query_mode>(mode);
    if ((selected_mode == query_mode::external_table &&
            (target == nullptr ||
                GetProcAddress(target, "inputnext_external_get_api") == nullptr)) ||
        (selected_mode == query_mode::external_callback && target == nullptr)) {
        return FALSE;
    }
    AcquireSRWLockExclusive(&state_lock);
    configured_mode = selected_mode;
    reentry_target = target;
    ReleaseSRWLockExclusive(&state_lock);
    return TRUE;
}

extern "C" void __cdecl inputnext_fixture_reset() noexcept {
    AcquireSRWLockExclusive(&inputnext::xinput_dll_fixture::state_lock);
    inputnext::xinput_dll_fixture::fixture_state = inputnext::xinput_dll_fixture::snapshot{};
    ReleaseSRWLockExclusive(&inputnext::xinput_dll_fixture::state_lock);
}

extern "C" BOOL __cdecl inputnext_fixture_get_snapshot(
    inputnext::xinput_dll_fixture::snapshot* const state) noexcept {
    if (state == nullptr) {
        return FALSE;
    }
    AcquireSRWLockExclusive(&inputnext::xinput_dll_fixture::state_lock);
    *state = inputnext::xinput_dll_fixture::fixture_state;
    ReleaseSRWLockExclusive(&inputnext::xinput_dll_fixture::state_lock);
    return TRUE;
}

extern "C" const input_next_runtime_api_v1* __cdecl inputnext_fixture_get_api() noexcept {
    return &inputnext::xinput_dll_fixture::valid_api;
}
