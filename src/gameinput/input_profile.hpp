#pragma once

#include <inputnext/device_metadata.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace inputnext::gameinput_detail {

enum class input_history_path : std::uint8_t { gamepad, callback_capture, composite };

enum class input_fallback_reason : std::uint8_t {
    none,
    unsupported_capability,
    incoherent_reading,
    missing_click,
    invalid_coordinates,
    history_lost,
    incomplete_replay,
    bootstrap_budget,
    epoch_changed,
};

enum class input_touch_mode : std::uint8_t { disabled, click_fallback, split };
enum class touch_coordinate_source : std::uint8_t { none, surface, dualsense_usb_report };

[[nodiscard]] constexpr std::string_view input_history_path_name(input_history_path path) noexcept {
    switch (path) {
    case input_history_path::gamepad:
        return "gamepad";
    case input_history_path::callback_capture:
        return "callback-capture";
    case input_history_path::composite:
        return "composite";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view input_fallback_reason_name(
    input_fallback_reason reason) noexcept {
    switch (reason) {
    case input_fallback_reason::none:
        return "none";
    case input_fallback_reason::unsupported_capability:
        return "unsupported-capability";
    case input_fallback_reason::incoherent_reading:
        return "incoherent-reading";
    case input_fallback_reason::missing_click:
        return "missing-click";
    case input_fallback_reason::invalid_coordinates:
        return "invalid-coordinates";
    case input_fallback_reason::history_lost:
        return "history-lost";
    case input_fallback_reason::incomplete_replay:
        return "incomplete-replay";
    case input_fallback_reason::bootstrap_budget:
        return "bootstrap-budget";
    case input_fallback_reason::epoch_changed:
        return "epoch-changed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view input_transport_name(
    physical_transport transport) noexcept {
    switch (transport) {
    case physical_transport::usb:
        return "usb";
    case physical_transport::bluetooth:
        return "bluetooth";
    case physical_transport::other_wireless:
        return "other-wireless";
    case physical_transport::other:
        return "other";
    case physical_transport::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view input_touch_mode_name(input_touch_mode mode) noexcept {
    switch (mode) {
    case input_touch_mode::disabled:
        return "disabled";
    case input_touch_mode::click_fallback:
        return "click-fallback";
    case input_touch_mode::split:
        return "split";
    }
    return "disabled";
}

// A connection-scoped description. Qualification is supplied by the native adapter, never by
// TOML or a caller-provided button index. Unknown combinations retain the gamepad-only path.
struct native_input_profile final {
    input_history_path history{input_history_path::gamepad};
    input_fallback_reason reason{input_fallback_reason::unsupported_capability};
    bool callback_coherent{};
    touch_coordinate_source coordinates{touch_coordinate_source::none};
    std::uint32_t button_count{};
    std::uint32_t click_button{};
    std::uint32_t surface_index{};
    std::uint32_t surface_width{};
    std::uint32_t surface_height{};
    std::array<std::uint16_t, 4U> runtime_version{};
    physical_transport transport{physical_transport::unknown};

    [[nodiscard]] constexpr bool touch_enabled() const noexcept {
        return callback_coherent && history != input_history_path::gamepad;
    }

    friend constexpr bool operator==(const native_input_profile&,
        const native_input_profile&) noexcept = default;
};

} // namespace inputnext::gameinput_detail
