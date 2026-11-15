#pragma once

#include "native_types.hpp"

#include <inputnext/gamepad_state.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace inputnext::dinput {

enum class object_kind : BYTE { axis, button, pov };
inline constexpr std::size_t axis_count = 6U;
inline constexpr std::size_t button_count = 15U;
inline constexpr std::size_t normalized_object_count = axis_count + button_count + 1U;

struct object_descriptor final {
    GUID guid{};
    DWORD id{};
    DWORD native_offset{};
    WORD usage_page{};
    WORD usage{};
    const wchar_t* name{};
    object_kind kind{};
    std::size_t source_index{};
};

[[nodiscard]] std::span<const object_descriptor> objects() noexcept;

struct axis_properties final {
    LONG minimum{};
    LONG maximum{65535};
    DWORD deadzone{};
    DWORD saturation{10000};
};

struct profile_values final {
    std::array<LONG, axis_count> axes{};
    std::array<BYTE, button_count> buttons{};
    DWORD pov{0xffffffffU};
};

// Stick saturation scales remaining travel after a centered deadzone. Trigger
// limits are unipolar and measured upward from the idle endpoint.
[[nodiscard]] profile_values project(const gamepad_state& source,
    const std::array<axis_properties, axis_count>& properties) noexcept;

} // namespace inputnext::dinput
