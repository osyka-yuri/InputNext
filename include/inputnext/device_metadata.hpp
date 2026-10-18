#pragma once

#include "device_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace inputnext {

enum class metadata_availability : std::uint8_t {
    available,
    not_present,
    unsupported,
    unavailable,
    ambiguous,
    capacity_exceeded,
};

enum class metadata_error : std::uint8_t {
    closed,
    not_connected,
    busy,
    stale,
    resource_exhausted,
};

template <typename T>
using metadata_result = std::expected<T, metadata_error>;

template <typename T>
struct metadata_field final {
    metadata_availability availability{metadata_availability::unavailable};
    T value{};
    std::uint32_t native_code{};

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return availability == metadata_availability::available;
    }
};

inline constexpr std::size_t gameinput_device_id_bytes = 32U;
inline constexpr std::size_t native_guid_bytes = 16U;
inline constexpr std::size_t max_pnp_path_utf8_bytes = 131'072U;
inline constexpr std::size_t headset_endpoint_buffer_code_units = 32'768U;
inline constexpr std::size_t headset_endpoint_content_code_units =
    headset_endpoint_buffer_code_units - 1U;
inline constexpr std::size_t haptic_endpoint_buffer_code_units = 256U;
inline constexpr std::size_t haptic_endpoint_content_code_units =
    haptic_endpoint_buffer_code_units - 1U;
inline constexpr std::size_t max_audio_endpoints_per_flow = 256U;

struct opaque_device_id final {
    std::array<std::byte, gameinput_device_id_bytes> bytes{};
    friend constexpr bool operator==(const opaque_device_id&,
        const opaque_device_id&) noexcept = default;
};

// Bytes use the native Windows GUID layout while keeping Windows SDK types out of portable headers.
struct native_guid final {
    std::array<std::byte, native_guid_bytes> bytes{};

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return std::ranges::all_of(bytes, [](const std::byte byte) { return byte == std::byte{}; });
    }

    friend constexpr bool operator==(const native_guid&, const native_guid&) noexcept = default;
};

enum class physical_transport : std::uint8_t {
    usb,
    bluetooth,
    other_wireless,
    other,
    unknown,
};

enum class transport_provenance : std::uint8_t {
    native_bus_type,
    native_enumerator,
    pnp_path_heuristic,
    unavailable,
};

enum class container_id_provenance : std::uint8_t {
    gameinput_native,
    exact_pnp_interface,
    conflicting,
    unavailable,
};

struct transport_metadata final {
    physical_transport value{physical_transport::unknown};
    transport_provenance provenance{transport_provenance::unavailable};
    friend constexpr bool operator==(const transport_metadata&,
        const transport_metadata&) noexcept = default;
};

// Normalized, copied GameInput facts. These describe hardware support and do not create output operations.
struct native_gameinput_capabilities final {
    bool gamepad{};
    bool low_frequency_rumble{};
    bool high_frequency_rumble{};
    bool left_trigger_rumble{};
    bool right_trigger_rumble{};
    friend constexpr bool operator==(const native_gameinput_capabilities&,
        const native_gameinput_capabilities&) noexcept = default;
};

enum class metadata_device_model : std::uint8_t {
    generic_gamepad,
    dualsense,
    dualsense_edge,
};

struct hardware_capabilities final {
    bool gamepad{};
    bool low_frequency_rumble{};
    bool high_frequency_rumble{};
    bool trigger_rumble{};
    bool adaptive_left_trigger{};
    bool adaptive_right_trigger{};
    bool lightbar{};
    bool player_leds{};
    bool microphone_led{};
    bool gyroscope{};
    bool accelerometer{};
    bool paddles{};
    friend constexpr bool operator==(const hardware_capabilities&,
        const hardware_capabilities&) noexcept = default;
};

enum class battery_state : std::uint8_t {
    unknown,
    charging,
    discharging,
    full,
};

struct native_device_facts final {
    metadata_field<std::uint16_t> vendor_id{};
    metadata_field<std::uint16_t> product_id{};
    metadata_field<std::uint16_t> revision{};
    metadata_field<opaque_device_id> gameinput_device_id{};
    metadata_field<opaque_device_id> gameinput_root_id{};
    metadata_field<native_guid> container_id{};
    // UTF-8, owned, and limited to max_pnp_path_utf8_bytes; exceeding the cap is reported explicitly.
    metadata_field<std::string> pnp_path_utf8{};
    metadata_field<native_gameinput_capabilities> gameinput_capabilities{};
    // UTF-16 content is limited to haptic_endpoint_content_code_units; this is never a headset ID.
    metadata_field<std::u16string> haptic_endpoint_id{};
};

struct audio_endpoint_metadata final {
    // UTF-16 content is limited to headset_endpoint_content_code_units.
    metadata_field<std::u16string> id{};
    // Optional DirectSound GUID. Failure to read it does not invalidate a valid endpoint ID.
    metadata_field<native_guid> direct_sound_guid{};
};

struct metadata_snapshot final {
    connection_target target{};
    std::uint64_t catalogue_generation{};
    native_device_facts native{};
    metadata_field<native_guid> container_id{};
    container_id_provenance container_id_source{container_id_provenance::unavailable};
    metadata_field<metadata_device_model> model{};
    metadata_field<hardware_capabilities> capabilities{};
    metadata_field<transport_metadata> transport{};
    // GameInput 4 exposes no battery telemetry; transport is not a charging state.
    metadata_field<battery_state> battery_telemetry{.availability =
                                                        metadata_availability::unsupported,
        .value = battery_state::unknown,
        .native_code = 0U};
    audio_endpoint_metadata headset_render{};
    audio_endpoint_metadata headset_capture{};
};

} // namespace inputnext
