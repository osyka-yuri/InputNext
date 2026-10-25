#include "body_values.hpp"
#include <cstdint>

#include "output/policy/body.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <optional>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::test::output::body_values;
using namespace inputnext;
using namespace inputnext::output;
using namespace inputnext::output::detail;
using namespace inputnext::output::detail::body_policy;
using output_error = inputnext::output::error;

TEST_CASE("output.body_policy_routes_preference_only_against_exact_audio_facts", "[unit][output]") {
    const auto target = test_target();
    const auto incarnation = test_incarnation();
    const auto audio = audio_instance{
        .target = target,
        .authority = incarnation,
        .id = audio_instance_id{3U},
    };
    const auto bound_identity = test_identity(audio);
    const auto unbound_identity = test_identity();

    const auto classic =
        plan_route(bound_identity, body_route_preference::classic, std::nullopt, false, 5U);
    CHECK(classic.route == body_actuator_route::classic);
    CHECK(classic.fallback == body_fallback_reason::none);

    const auto unbound =
        plan_route(unbound_identity, body_route_preference::prefer_hd, std::nullopt, false, 6U);
    CHECK(unbound.route == body_actuator_route::classic);
    CHECK(unbound.fallback == body_fallback_reason::not_ready);

    const auto missing =
        plan_route(bound_identity, body_route_preference::prefer_hd, std::nullopt, false, 7U);
    CHECK(missing.fallback == body_fallback_reason::instance_lost);

    const auto unsupported = plan_route(bound_identity,
        body_route_preference::prefer_hd,
        audio_route_facts{.instance = audio,
            .support = body_route_support::classic_only,
            .ready = true},
        false,
        8U);
    CHECK(unsupported.fallback == body_fallback_reason::unsupported_transport);

    const auto not_ready = plan_route(bound_identity,
        body_route_preference::prefer_hd,
        audio_route_facts{.instance = audio, .support = body_route_support::hd, .ready = false},
        false,
        9U);
    CHECK(not_ready.fallback == body_fallback_reason::not_ready);

    const auto ready = plan_route(bound_identity,
        body_route_preference::prefer_hd,
        audio_route_facts{.instance = audio, .support = body_route_support::hd, .ready = true},
        false,
        10U);
    CHECK(ready.route == body_actuator_route::hd);
    CHECK(ready.fallback == body_fallback_reason::none);

    const auto diagnostic = plan_route(bound_identity,
        body_route_preference::prefer_hd,
        audio_route_facts{.instance = audio, .support = body_route_support::hd, .ready = true},
        true,
        11U);
    CHECK(diagnostic.route == body_actuator_route::classic);
    CHECK(diagnostic.fallback == body_fallback_reason::preempted);
}

TEST_CASE("output.body_policy_validates_finite_leases_and_nonwrapping_sequences",
    "[unit][output]") {
    body_request request{.sequence = 1U, .ttl = 1ms};
    CHECK(validate_request(request).has_value());

    request.ttl = maximum_finite_ttl;
    CHECK(validate_request(request).has_value());
    request.ttl = 0ms;
    CHECK(validate_request(request).error() == output_error::invalid_argument);
    request.ttl = maximum_finite_ttl + 1ms;
    CHECK(validate_request(request).error() == output_error::invalid_argument);

    request.ttl = 1ms;
    request.sequence = 0U;
    CHECK(validate_request(request).error() == output_error::invalid_argument);
    request.sequence = std::numeric_limits<std::uint64_t>::max();
    CHECK(validate_request(request).has_value());
    request.fallback.low_frequency = std::numeric_limits<float>::infinity();
    CHECK(validate_request(request).error() == output_error::invalid_argument);
    request.fallback = {};
    request.audio.id = audio_instance_id{1U};
    CHECK(validate_request(request).error() == output_error::invalid_argument);

    CHECK(!(valid_route_support(std::bit_cast<body_route_support>(std::uint8_t{0xFFU}))));
    CHECK(!(valid_preference(std::bit_cast<body_route_preference>(std::uint8_t{0xFFU}))));
}

} // namespace
