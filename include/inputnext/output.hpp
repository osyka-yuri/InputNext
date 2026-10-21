#pragma once

#include <inputnext/device_identity.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace inputnext::output {

enum class error : std::uint8_t {
    invalid_argument,
    not_connected,
    resources,
    capacity_exceeded,
    identity_exhausted,
    sequence_rejected,
    revision_exhausted,
    logical_version_exhausted,
    stale_context,
    revoked,
    stale_revision,
    expired,
    deadline_overflow,
};

template <typename T>
using result = std::expected<T, error>;

enum class owner_class : std::uint8_t {
    public_game,
    trusted_internal,
};

struct authority_incarnation final {
    std::array<std::byte, 16U> bytes{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return std::ranges::any_of(bytes, [](const std::byte byte) { return byte != std::byte{}; });
    }

    friend constexpr bool operator==(const authority_incarnation&,
        const authority_incarnation&) noexcept = default;
};

struct context_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(const context_id&, const context_id&) noexcept = default;
};

struct internal_owner_key final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(const internal_owner_key&,
        const internal_owner_key&) noexcept = default;
};

struct context_identity final {
    connection_target target{};
    authority_incarnation incarnation{};
    context_id id{};
    owner_class kind{owner_class::public_game};
    internal_owner_key internal_owner{};

    friend constexpr bool operator==(const context_identity&,
        const context_identity&) noexcept = default;
};

struct public_context final {
    context_identity identity{};
    friend constexpr bool operator==(const public_context&,
        const public_context&) noexcept = default;
};

struct internal_context final {
    context_identity identity{};
    friend constexpr bool operator==(const internal_context&,
        const internal_context&) noexcept = default;
};

enum class internal_priority : std::uint8_t {
    lowest = 0U,
    generic_profile = 20U,
    game_profile = 40U,
    game_semantic = 60U,
    diagnostic = 100U,
};

enum class ttl_permission : std::uint8_t {
    finite_only,
    finite_or_indefinite,
};

enum class foreground_policy : std::uint8_t {
    background,
    foreground_bound,
};

struct internal_context_options final {
    internal_owner_key owner{};
    internal_priority priority{internal_priority::lowest};
    ttl_permission ttl{ttl_permission::finite_only};
    foreground_policy foreground{foreground_policy::background};

    friend constexpr bool operator==(const internal_context_options&,
        const internal_context_options&) noexcept = default;
};

enum class channel : std::uint8_t {
    left_trigger,
    right_trigger,
    rumble,
    leds,
};

using channel_mask = std::uint32_t;
inline constexpr channel_mask left_trigger_mask = 1U << 0U;
inline constexpr channel_mask right_trigger_mask = 1U << 1U;
inline constexpr channel_mask rumble_mask = 1U << 2U;
inline constexpr channel_mask leds_mask = 1U << 3U;
inline constexpr channel_mask supported_channel_mask =
    left_trigger_mask | right_trigger_mask | rumble_mask | leds_mask;
inline constexpr std::size_t channel_count = 4U;
inline constexpr std::chrono::milliseconds maximum_finite_ttl{60000};

[[nodiscard]] constexpr channel_mask mask_for(const channel value) noexcept {
    switch (value) {
    case channel::left_trigger:
        return left_trigger_mask;
    case channel::right_trigger:
        return right_trigger_mask;
    case channel::rumble:
        return rumble_mask;
    case channel::leds:
        return leds_mask;
    }
    return 0U;
}

enum class trigger_mode : std::uint8_t {
    off,
    feedback,
    weapon,
    vibration,
};

enum class trigger_layout : std::uint8_t {
    uniform,
    per_zone,
};

inline constexpr std::size_t trigger_zone_count = 10U;
inline constexpr std::uint8_t maximum_trigger_zone_level = 8U;
inline constexpr float maximum_trigger_frequency = 255.0F;
inline constexpr std::uint8_t maximum_player_leds = 4U;

struct trigger_value final {
    trigger_mode mode{trigger_mode::off};
    trigger_layout layout{trigger_layout::uniform};
    float start{};
    float end{};
    float strength{};
    float frequency{};
    std::array<std::uint8_t, trigger_zone_count> zones{};

    friend constexpr bool operator==(const trigger_value&, const trigger_value&) noexcept = default;
};

struct rumble_value final {
    float low_frequency{};
    float high_frequency{};

    friend constexpr bool operator==(const rumble_value&, const rumble_value&) noexcept = default;
};

struct led_value final {
    std::array<std::uint8_t, 3U> rgb{};
    std::uint8_t player_leds{};
    bool microphone_led{};

    friend constexpr bool operator==(const led_value&, const led_value&) noexcept = default;
};

struct channel_values final {
    trigger_value left_trigger{};
    trigger_value right_trigger{};
    rumble_value rumble{};
    led_value leds{};

    friend constexpr bool operator==(const channel_values&,
        const channel_values&) noexcept = default;
};

struct submit_request final {
    channel_mask selected_channels{};
    std::uint64_t sequence{};
    std::chrono::milliseconds ttl{};
    channel_values values{};
};

using channel_revisions = std::array<std::uint64_t, channel_count>;

struct clear_request final {
    channel_mask selected_channels{};
    std::uint64_t sequence{};
    channel_revisions expected_revisions{};
};

struct mutation_receipt final {
    context_identity identity{};
    std::uint64_t accepted_sequence{};
    channel_revisions resulting_revisions{};
    std::uint64_t logical_version{};

    friend constexpr bool operator==(const mutation_receipt&,
        const mutation_receipt&) noexcept = default;
};

enum class close_reason : std::uint8_t {
    explicit_close,
    foreground_revocation,
};

struct close_receipt final {
    context_identity identity{};
    context_id close_request_id{};
    std::uint64_t logical_revocation_version{};
    close_reason reason{close_reason::explicit_close};

    friend constexpr bool operator==(const close_receipt&, const close_receipt&) noexcept = default;
};

} // namespace inputnext::output
