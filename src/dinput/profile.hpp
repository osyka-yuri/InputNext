#pragma once

#include "native_types.hpp"

#include <inputnext/gamepad_state.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace inputnext::dinput {

enum class object_kind : BYTE { axis, button, pov };
inline constexpr std::size_t axis_count = 6U;
inline constexpr std::size_t pov_count = 1U;
inline constexpr std::size_t standard_button_count = 15U;
inline constexpr std::size_t max_button_count = standard_button_count + 2U;
inline constexpr std::size_t max_object_count = axis_count + max_button_count + pov_count;

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
    std::array<BYTE, max_button_count> buttons{};
    DWORD pov{0xffffffffU};
};

class device_model final {
public:
    constexpr device_model() noexcept = default;
    explicit device_model(bool digital_triggers, double trigger_threshold = 0.5) noexcept;

    [[nodiscard]] constexpr std::size_t button_count() const noexcept {
        return digital_triggers_ ? max_button_count : standard_button_count;
    }

    [[nodiscard]] std::span<const object_descriptor> objects() const noexcept;

    // Analog dead zones are centered for sticks and start at idle for triggers.
    // Saturation applies to the travel beyond the dead zone.
    [[nodiscard]] profile_values project(const gamepad_state& source,
        const std::array<axis_properties, axis_count>& properties) const noexcept;

private:
    bool digital_triggers_{};
    float trigger_threshold_{0.5F};
};

} // namespace inputnext::dinput
