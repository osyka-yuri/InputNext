#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

namespace inputnext::xinput_dll_fixture {

enum class callback_id : std::uint8_t {
    get_state = 1U,
    set_state = 2U,
    get_capabilities = 3U,
    enable = 4U,
    get_battery_information = 5U,
    get_keystroke = 6U,
    get_audio_device_ids = 7U,
    get_dsound_audio_device_guids = 8U,
    get_state_ex = 9U,
    get_capabilities_ex = 10U,
    wait_for_guide_button = 11U,
    cancel_guide_button_wait = 12U,
    power_off_controller = 13U,
};

enum class query_mode : std::uint8_t {
    valid = 0U,
    invalid_size = 1U,
    invalid_version = 2U,
    missing_callback = 3U,
    null_table = 4U,
    reenter_same_facade = 5U,
    reenter_cross_facade = 6U,
    external_table = 7U,
    external_callback = 8U,
};

struct snapshot final {
    std::uint32_t query_calls{};
    std::uint32_t callback_calls{};
    std::uint32_t last_callback{};
    std::uint32_t last_flavor{};
    std::uint32_t last_requested_abi{};
    std::array<std::uintptr_t, 6U> last_arguments{};
    DWORD last_callback_result{};
    DWORD reentry_result{};
    std::uint32_t reentry_calls{};
};

using configure_fn = BOOL(__cdecl*)(std::uint32_t mode, HMODULE target_module) noexcept;
using reset_fn = void(__cdecl*)() noexcept;
using snapshot_fn = BOOL(__cdecl*)(snapshot* state) noexcept;

inline constexpr DWORD callback_result_base = 0xA500'0000U;

[[nodiscard]] constexpr DWORD expected_result(const callback_id id) noexcept {
    return callback_result_base | static_cast<DWORD>(id);
}

} // namespace inputnext::xinput_dll_fixture
