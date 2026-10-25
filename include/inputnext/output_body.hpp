#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include <chrono>
#include <cstdint>

namespace inputnext::output {

struct body_lease_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(body_lease_id, body_lease_id) noexcept = default;
};

struct audio_instance_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(audio_instance_id, audio_instance_id) noexcept = default;
};

struct audio_instance final {
    connection_target target{};
    authority_incarnation authority{};
    audio_instance_id id{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return target.valid() && authority.valid() && id.valid();
    }

    [[nodiscard]] constexpr bool unbound() const noexcept {
        return target == connection_target{} && authority == authority_incarnation{} &&
               id == audio_instance_id{};
    }

    friend constexpr bool operator==(const audio_instance&,
        const audio_instance&) noexcept = default;
};

struct body_lease_identity final {
    context_identity context{};
    body_lease_id lease{};
    audio_instance audio{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        const bool valid_context = context.target.valid() && context.incarnation.valid() &&
                                   context.id.valid() && context.kind == owner_class::public_game &&
                                   !context.internal_owner.valid();
        const bool valid_audio =
            audio.unbound() || (audio.valid() && audio.target == context.target &&
                                   audio.authority == context.incarnation);
        return valid_context && lease.valid() && valid_audio;
    }

    friend constexpr bool operator==(const body_lease_identity&,
        const body_lease_identity&) noexcept = default;
};

enum class body_route_preference : std::uint8_t {
    classic,
    prefer_hd,
};

enum class body_actuator_route : std::uint8_t {
    classic,
    hd,
};

enum class body_route_support : std::uint8_t {
    classic_only,
    hd,
};

enum class body_fallback_reason : std::uint8_t {
    none,
    not_ready,
    unsupported_transport,
    instance_lost,
    output_failed,
    preempted,
};

struct body_request final {
    body_lease_id lease{};
    audio_instance audio{};
    std::uint64_t sequence{};
    std::chrono::milliseconds ttl{};
    body_route_preference preference{body_route_preference::classic};
    rumble_value fallback{};
};

struct body_clear_request final {
    body_lease_id lease{};
    audio_instance audio{};
    std::uint64_t sequence{};
};

struct body_route_intent final {
    body_lease_identity identity{};
    std::uint64_t revision{};
    body_actuator_route route{body_actuator_route::classic};
    body_fallback_reason fallback{body_fallback_reason::none};

    friend constexpr bool operator==(const body_route_intent&,
        const body_route_intent&) noexcept = default;
};

struct body_snapshot final {
    body_route_intent intent{};
    std::uint64_t accepted_sequence{};
    std::chrono::steady_clock::time_point expires_at;
    rumble_value fallback_rumble{};

    friend constexpr bool operator==(const body_snapshot&, const body_snapshot&) noexcept = default;
};

struct body_receipt final {
    body_lease_identity identity{};
    std::uint64_t accepted_sequence{};
    std::uint64_t logical_version{};
    body_route_intent intent{};

    friend constexpr bool operator==(const body_receipt&, const body_receipt&) noexcept = default;
};

} // namespace inputnext::output
