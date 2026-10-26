#include "audio/native_types.hpp"
#include "audio/windows/endpoint_snapshot.hpp"
#include "audio/windows_endpoint.hpp"

#include <inputnext/device_metadata.hpp>

#include <Windows.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace inputnext::audio_detail::windows {
namespace {

using Microsoft::WRL::Make;
using test::audio::controlled_endpoint;
using test::audio::controlled_property_store;

TEST_CASE("audio.windows_endpoint.snapshot_releases_temporary_interfaces",
    "[unit][audio][windows]") {
    const auto properties = Make<controlled_property_store>();
    REQUIRE(properties.Get() != nullptr);
    const auto device = Make<controlled_endpoint>(properties);
    REQUIRE(device.Get() != nullptr);
    const auto device_refs = device->references();
    const auto property_refs = properties->references();

    const auto snapshot = inspect_endpoint(*device.Get());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->id.view() == u"endpoint-A");
    CHECK(snapshot->state == native_endpoint_state::active);
    CHECK(snapshot->flow == native_endpoint_flow::render);
    CHECK(std::memcmp(snapshot->container_id.bytes.data(), &properties->container, sizeof(GUID)) ==
          0);
    CHECK(properties->reads == 1U);
    CHECK(device->store_opens == 1U);
    CHECK(device->references_at_store_open == device_refs);
    CHECK(device->references() == device_refs);
    CHECK(properties->references() == property_refs);
}

TEST_CASE("audio.windows_endpoint.failures_release_borrowed_interfaces", "[unit][audio][windows]") {
    constexpr std::array operations{native_operation::get_endpoint_id,
        native_operation::get_endpoint_state,
        native_operation::get_endpoint_flow,
        native_operation::open_property_store,
        native_operation::get_container_id};
    for (const auto operation : operations) {
        CAPTURE(operation);
        const auto properties = Make<controlled_property_store>();
        REQUIRE(properties.Get() != nullptr);
        const auto device = Make<controlled_endpoint>(properties);
        REQUIRE(device.Get() != nullptr);
        const auto device_refs = device->references();
        const auto property_refs = properties->references();
        switch (operation) {
        case native_operation::get_endpoint_id:
            device->id_result = E_FAIL;
            break;
        case native_operation::get_endpoint_state:
            device->state_result = E_FAIL;
            break;
        case native_operation::get_endpoint_flow:
            device->flow_result = E_FAIL;
            break;
        case native_operation::open_property_store:
            device->store_result = E_FAIL;
            break;
        case native_operation::get_container_id:
            properties->value_result = E_FAIL;
            break;
        default:
            FAIL("invalid endpoint failure injection");
        }
        const auto snapshot = inspect_endpoint(*device.Get());
        REQUIRE(!snapshot.has_value());
        const native_error expected{.operation = operation,
            .native_code = static_cast<std::uint32_t>(E_FAIL)};
        CHECK(snapshot.error() == expected);
        CHECK(device->references() == device_refs);
        CHECK(properties->references() == property_refs);
    }
}

TEST_CASE("audio.windows_endpoint.rejects_incomplete_identity_without_retaining_properties",
    "[unit][audio][windows]") {
    for (unsigned int scenario = 0U; scenario < 4U; ++scenario) {
        CAPTURE(scenario);
        const auto properties = Make<controlled_property_store>();
        REQUIRE(properties.Get() != nullptr);
        const auto device = Make<controlled_endpoint>(properties);
        REQUIRE(device.Get() != nullptr);
        const auto property_refs = properties->references();
        if (scenario == 0U) {
            device->null_id = true;
        } else if (scenario == 1U) {
            device->null_store = true;
        } else if (scenario == 2U) {
            properties->value_type = VT_EMPTY;
        } else {
            properties->null_container = true;
        }
        const auto snapshot = inspect_endpoint(*device.Get());
        CHECK(!snapshot.has_value());
        CHECK(device->references() == 1U);
        CHECK(properties->references() == property_refs);
    }
}

TEST_CASE("audio.windows_endpoint.validates_native_id_bounds", "[unit][audio][windows]") {
    const auto properties = Make<controlled_property_store>();
    REQUIRE(properties.Get() != nullptr);
    const auto device = Make<controlled_endpoint>(properties);
    REQUIRE(device.Get() != nullptr);
    device->id.assign(haptic_endpoint_content_code_units, L'x');
    const auto maximum = inspect_endpoint(*device.Get());
    REQUIRE(maximum.has_value());
    CHECK(maximum->id.size() == haptic_endpoint_content_code_units);
    CHECK(maximum->id.terminated_data()[maximum->id.size()] == u'\0');

    device->id.push_back(L'x');
    const auto oversized = inspect_endpoint(*device.Get());
    REQUIRE(!oversized.has_value());
    CHECK(oversized.error().operation == native_operation::get_endpoint_id);
    CHECK(oversized.error().native_code == ERROR_INSUFFICIENT_BUFFER);
}

} // namespace
} // namespace inputnext::audio_detail::windows
