#include "output/authority_fixture.hpp"
#include <inputnext/output_body.hpp>

#include "execution_engine_scope.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include <memory>
#include <optional>

namespace {

using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_native_sink;
using inputnext::test::output::controlled_native_state;
using inputnext::test::output::execution_engine_scope;

TEST_CASE("output.body_route_hd_registration_requires_rumble_representation",
    "[component][output][execution][body_route]") {
    authority_fixture fixture;
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());

    const auto reservation = scope.engine().reserve_backend(fixture.connection.target,
        candidate,
        left_trigger_mask | leds_mask,
        std::nullopt,
        body_route_support::hd);

    REQUIRE_FALSE(reservation.has_value());
    CHECK(reservation.error() == execution_error::invalid_argument);
    CHECK(candidate != nullptr);
}

TEST_CASE("output.body_route_registration_rejects_unknown_support_without_consuming_sink",
    "[component][output][execution][body_route]") {
    authority_fixture fixture;
    execution_engine_scope scope{fixture.owner};
    std::unique_ptr<native_sink> candidate =
        std::make_unique<controlled_native_sink>(std::make_shared<controlled_native_state>());

    const auto reservation = scope.engine().reserve_backend(fixture.connection.target,
        candidate,
        rumble_mask,
        std::nullopt,
        std::bit_cast<body_route_support>(std::uint8_t{0xFFU}));

    REQUIRE_FALSE(reservation.has_value());
    CHECK(reservation.error() == execution_error::invalid_argument);
    CHECK(candidate != nullptr);
}

} // namespace
