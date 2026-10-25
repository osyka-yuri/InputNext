#include "body.hpp"
#include <inputnext/output_body.hpp>

#include <inputnext/output.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <optional>

namespace inputnext::output::detail::body_policy {
namespace {

[[nodiscard]] bool valid_audio(const audio_instance& value) noexcept {
    return value.unbound() || value.valid();
}

[[nodiscard]] bool valid_rumble(const rumble_value value) noexcept {
    return std::isfinite(value.low_frequency) && value.low_frequency >= 0.0F &&
           value.low_frequency <= 1.0F && std::isfinite(value.high_frequency) &&
           value.high_frequency >= 0.0F && value.high_frequency <= 1.0F;
}

} // namespace

bool valid_preference(const body_route_preference value) noexcept {
    switch (value) {
    case body_route_preference::classic:
    case body_route_preference::prefer_hd:
        return true;
    }
    return false;
}

bool valid_route_support(const body_route_support value) noexcept {
    switch (value) {
    case body_route_support::classic_only:
    case body_route_support::hd:
        return true;
    }
    return false;
}

result<void> validate_request(const body_request& request) noexcept {
    if (!valid_audio(request.audio) || !valid_preference(request.preference) ||
        request.sequence == 0U || request.ttl.count() < 1 || request.ttl > maximum_finite_ttl ||
        !valid_rumble(request.fallback)) {
        return std::unexpected{error::invalid_argument};
    }
    return {};
}

result<void> validate_clear_request(const body_clear_request& request) noexcept {
    if (!request.lease.valid() || !valid_audio(request.audio) || request.sequence == 0U) {
        return std::unexpected{error::invalid_argument};
    }
    return {};
}

body_route_intent plan_route(const body_lease_identity identity,
    const body_route_preference preference,
    const std::optional<audio_route_facts> audio,
    const bool diagnostic_winner,
    const std::uint64_t revision) noexcept {
    body_route_intent intent{.identity = identity, .revision = revision};
    if (diagnostic_winner) {
        intent.fallback = body_fallback_reason::preempted;
        return intent;
    }
    if (preference == body_route_preference::classic) {
        return intent;
    }
    if (identity.audio.unbound()) {
        intent.fallback = body_fallback_reason::not_ready;
        return intent;
    }
    if (!audio.has_value() || audio->instance != identity.audio) {
        intent.fallback = body_fallback_reason::instance_lost;
        return intent;
    }
    if (audio->support == body_route_support::classic_only) {
        intent.fallback = body_fallback_reason::unsupported_transport;
        return intent;
    }
    if (!audio->ready) {
        intent.fallback = body_fallback_reason::not_ready;
        return intent;
    }
    intent.route = body_actuator_route::hd;
    return intent;
}

body_route_intent fallback_route(const body_route_intent& current,
    const body_fallback_reason reason,
    const std::uint64_t revision) noexcept {
    auto fallback = current;
    fallback.revision = revision;
    fallback.route = body_actuator_route::classic;
    fallback.fallback = reason;
    return fallback;
}

body_route_intent represented_route(const body_snapshot& body,
    const body_route_support support,
    const bool diagnostic_winner) noexcept {
    auto route = body.intent;
    if (diagnostic_winner) {
        route.route = body_actuator_route::classic;
        route.fallback = body_fallback_reason::preempted;
    } else if (route.route == body_actuator_route::hd &&
               support == body_route_support::classic_only) {
        route.route = body_actuator_route::classic;
        route.fallback = body_fallback_reason::unsupported_transport;
    }
    return route;
}

rumble_value represented_rumble(const body_snapshot& body,
    const body_route_support support) noexcept {
    if (body.intent.route == body_actuator_route::hd && support == body_route_support::hd) {
        return {};
    }
    return body.fallback_rumble;
}

} // namespace inputnext::output::detail::body_policy
