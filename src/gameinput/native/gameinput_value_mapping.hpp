#pragma once

#include <GameInput.h>

#include <inputnext/device_metadata.hpp>
#include <inputnext/gamepad_state.hpp>

#include <cstdint>

namespace inputnext::gameinput_detail {

enum class haptic_info_result : std::uint8_t {
    available,
    not_present,
    failure,
};

[[nodiscard]] haptic_info_result classify_haptic_info_result(HRESULT result) noexcept;
[[nodiscard]] gamepad_state map_gamepad_state(
    const GameInput::v4::GameInputGamepadState& native_state) noexcept;
[[nodiscard]] opaque_device_id copy_device_id(const APP_LOCAL_DEVICE_ID& native_id) noexcept;
[[nodiscard]] native_guid copy_guid(const GUID& value) noexcept;

} // namespace inputnext::gameinput_detail
