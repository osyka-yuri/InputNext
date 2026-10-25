#pragma once

#include <cstdint>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <optional>

namespace inputnext::output::detail::body_policy {

struct audio_route_facts final {
    audio_instance instance{};
    body_route_support support{body_route_support::classic_only};
    bool ready{};
};

[[nodiscard]] bool valid_preference(body_route_preference value) noexcept;
[[nodiscard]] bool valid_route_support(body_route_support value) noexcept;
[[nodiscard]] result<void> validate_request(const body_request& request) noexcept;
[[nodiscard]] result<void> validate_clear_request(const body_clear_request& request) noexcept;
[[nodiscard]] body_route_intent plan_route(body_lease_identity identity,
    body_route_preference preference,
    std::optional<audio_route_facts> audio,
    bool diagnostic_winner,
    std::uint64_t revision) noexcept;
[[nodiscard]] body_route_intent fallback_route(const body_route_intent& current,
    body_fallback_reason reason,
    std::uint64_t revision) noexcept;
[[nodiscard]] body_route_intent represented_route(const body_snapshot& body,
    body_route_support support,
    bool diagnostic_winner) noexcept;
[[nodiscard]] rumble_value represented_rumble(const body_snapshot& body,
    body_route_support support) noexcept;

} // namespace inputnext::output::detail::body_policy
