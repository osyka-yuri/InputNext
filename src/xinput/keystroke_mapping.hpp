#pragma once

#include <Windows.h>
#include <Xinput.h>

#include <cstdint>

namespace inputnext::xinput::detail {

using key_mask = std::uint32_t;

inline constexpr std::uint32_t keystroke_key_count = 32U;

[[nodiscard]] key_mask map_keystroke_keys(const XINPUT_GAMEPAD& gamepad) noexcept;

} // namespace inputnext::xinput::detail
