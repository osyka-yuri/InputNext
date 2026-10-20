#pragma once

#include <inputnext/gamepad_state.hpp>

#include <Windows.h>
#include <Xinput.h>

namespace inputnext::xinput::detail {

[[nodiscard]] SHORT to_xinput_axis(float value) noexcept;
[[nodiscard]] BYTE to_xinput_trigger(float value) noexcept;
[[nodiscard]] WORD to_xinput_buttons(button_mask buttons, bool include_guide) noexcept;
void translate_state(const gamepad_state& source,
    XINPUT_GAMEPAD& destination,
    bool include_guide) noexcept;
[[nodiscard]] bool same_gamepad(const XINPUT_GAMEPAD& left, const XINPUT_GAMEPAD& right) noexcept;

} // namespace inputnext::xinput::detail
