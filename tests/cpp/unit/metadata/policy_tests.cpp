#include <inputnext/device_metadata.hpp>

#include <metadata/policy.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace inputnext;
using namespace inputnext::metadata_detail;

namespace {

[[nodiscard]] std::vector<char16_t> terminated_text(const std::size_t content_units,
    const char16_t value) {
    std::vector<char16_t> result(content_units + 1U, value);
    result.back() = u'\0';
    return result;
}

[[nodiscard]] native_device_facts dualsense_facts(const std::uint16_t product) {
    native_device_facts facts{};
    facts.vendor_id = {.availability = metadata_availability::available,
        .value = 0x054CU,
        .native_code = 0U};
    facts.product_id = {.availability = metadata_availability::available,
        .value = product,
        .native_code = 0U};
    return facts;
}

} // namespace

TEST_CASE("metadata.bounds.headset_utf16_uses_buffer_including_nul", "[unit][metadata][bounds]") {
    const auto maximum = terminated_text(headset_endpoint_content_code_units, u'x');
    const auto accepted = copy_bounded_utf16({.data = maximum.data(), .code_units = maximum.size()},
        headset_endpoint_content_code_units);
    REQUIRE(accepted.has_value());
    CHECK(accepted.value.size() == headset_endpoint_content_code_units);

    const auto over_limit = terminated_text(headset_endpoint_content_code_units + 1U, u'x');
    const auto rejected =
        copy_bounded_utf16({.data = over_limit.data(), .code_units = over_limit.size()},
            headset_endpoint_content_code_units);
    CHECK(rejected.availability == metadata_availability::capacity_exceeded);
    CHECK(rejected.value.empty());

    const std::array<char16_t, 1U> empty{u'\0'};
    CHECK(copy_bounded_utf16({.data = empty.data(), .code_units = empty.size()},
              headset_endpoint_content_code_units)
              .availability == metadata_availability::not_present);
    CHECK(copy_bounded_utf16({.data = nullptr, .code_units = empty.size()},
              headset_endpoint_content_code_units)
              .availability == metadata_availability::unavailable);

    const std::array<char16_t, 3U> unterminated{u'a', u'b', u'c'};
    const auto malformed =
        copy_bounded_utf16({.data = unterminated.data(), .code_units = unterminated.size()}, 2U);
    CHECK(malformed.availability == metadata_availability::unavailable);
    CHECK(malformed.value.empty());
}

TEST_CASE("metadata.bounds.haptic_utf16_uses_its_smaller_buffer", "[unit][metadata][bounds]") {
    const auto maximum = terminated_text(haptic_endpoint_content_code_units, u'h');
    const auto accepted = copy_bounded_utf16({.data = maximum.data(), .code_units = maximum.size()},
        haptic_endpoint_content_code_units);
    REQUIRE(accepted.has_value());
    CHECK(accepted.value.size() == haptic_endpoint_content_code_units);

    const auto over_limit = terminated_text(haptic_endpoint_content_code_units + 1U, u'h');
    const auto rejected =
        copy_bounded_utf16({.data = over_limit.data(), .code_units = over_limit.size()},
            haptic_endpoint_content_code_units);
    CHECK(rejected.availability == metadata_availability::capacity_exceeded);
    CHECK(rejected.value.empty());

    const auto empty_content = copy_bounded_utf16_content({}, haptic_endpoint_content_code_units);
    CHECK(empty_content.availability == metadata_availability::not_present);
    const std::u16string embedded_nul{u'a', u'\0', u'b'};
    CHECK(
        copy_bounded_utf16_content(embedded_nul, haptic_endpoint_content_code_units).availability ==
        metadata_availability::unavailable);
}

TEST_CASE("metadata.bounds.pnp_path_has_an_explicit_utf8_byte_cap", "[unit][metadata][bounds]") {
    const std::string maximum(max_pnp_path_utf8_bytes, 'x');
    const auto accepted = copy_bounded_pnp_path(maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted.value.size() == max_pnp_path_utf8_bytes);

    const std::string over_limit(max_pnp_path_utf8_bytes + 1U, 'x');
    const auto rejected = copy_bounded_pnp_path(over_limit);
    CHECK(rejected.availability == metadata_availability::capacity_exceeded);
    CHECK(rejected.value.empty());
    CHECK(copy_bounded_pnp_path({}).availability == metadata_availability::not_present);

    const std::string invalid_utf8{"\xC0\x80", 2U};
    CHECK(copy_bounded_pnp_path(invalid_utf8).availability == metadata_availability::unavailable);
    const std::string embedded_nul{"USB\0path", 8U};
    CHECK(copy_bounded_pnp_path(embedded_nul).availability == metadata_availability::unavailable);
}

TEST_CASE("metadata.policy.transport_heuristics_preserve_their_provenance",
    "[unit][metadata][policy]") {
    const auto usb = infer_transport_from_pnp_path(R"(\\?\HID#VID_054C&PID_0CE6#usb)");
    REQUIRE(usb.has_value());
    CHECK(usb.value.value == physical_transport::usb);
    CHECK(usb.value.provenance == transport_provenance::pnp_path_heuristic);

    const auto bluetooth = infer_transport_from_pnp_path(R"(\\?\BTHENUM#device)");
    REQUIRE(bluetooth.has_value());
    CHECK(bluetooth.value.value == physical_transport::bluetooth);

    const auto other_wireless = infer_transport_from_pnp_path(R"(\\?\ACPI#wireless-controller)");
    REQUIRE(other_wireless.has_value());
    CHECK(other_wireless.value.value == physical_transport::other_wireless);

    const auto unknown = infer_transport_from_pnp_path(R"(\\?\HID#controller)");
    CHECK(unknown.availability == metadata_availability::unavailable);
    CHECK(unknown.value.value == physical_transport::unknown);
    CHECK(unknown.value.provenance == transport_provenance::unavailable);
}

TEST_CASE("metadata.policy.capabilities_copy_model_facts_without_creating_operations",
    "[unit][metadata][policy]") {
    const auto dualsense = dualsense_facts(0x0CE6U);
    const auto dualsense_model = classify_device_model(dualsense);
    REQUIRE(dualsense_model.has_value());
    CHECK(dualsense_model.value == metadata_device_model::dualsense);
    const auto dualsense_capabilities = derive_hardware_capabilities(dualsense);
    REQUIRE(dualsense_capabilities.has_value());
    CHECK(dualsense_capabilities.value.gamepad);
    CHECK(dualsense_capabilities.value.adaptive_left_trigger);
    CHECK(dualsense_capabilities.value.adaptive_right_trigger);
    CHECK(dualsense_capabilities.value.lightbar);
    CHECK(dualsense_capabilities.value.player_leds);
    CHECK(dualsense_capabilities.value.microphone_led);
    CHECK(dualsense_capabilities.value.gyroscope);
    CHECK(dualsense_capabilities.value.accelerometer);
    CHECK_FALSE(dualsense_capabilities.value.paddles);

    const auto edge = dualsense_facts(0x0DF2U);
    const auto edge_capabilities = derive_hardware_capabilities(edge);
    REQUIRE(edge_capabilities.has_value());
    CHECK(edge_capabilities.value.paddles);
    CHECK(edge_capabilities.value.adaptive_left_trigger);

    native_device_facts generic{};
    generic.vendor_id = {.availability = metadata_availability::available,
        .value = 0x1234U,
        .native_code = 0U};
    generic.product_id = {.availability = metadata_availability::available,
        .value = 0x5678U,
        .native_code = 0U};
    generic.gameinput_capabilities = {.availability = metadata_availability::available,
        .value = native_gameinput_capabilities{.gamepad = true,
            .low_frequency_rumble = false,
            .high_frequency_rumble = true,
            .left_trigger_rumble = false,
            .right_trigger_rumble = true},
        .native_code = 0U};
    const auto generic_model = classify_device_model(generic);
    REQUIRE(generic_model.has_value());
    CHECK(generic_model.value == metadata_device_model::generic_gamepad);
    const auto generic_capabilities = derive_hardware_capabilities(generic);
    REQUIRE(generic_capabilities.has_value());
    CHECK(generic_capabilities.value.gamepad);
    CHECK_FALSE(generic_capabilities.value.low_frequency_rumble);
    CHECK(generic_capabilities.value.high_frequency_rumble);
    CHECK(generic_capabilities.value.trigger_rumble);
    CHECK_FALSE(generic_capabilities.value.adaptive_left_trigger);
    CHECK_FALSE(generic_capabilities.value.paddles);

    const auto unsupported_caps = derive_hardware_capabilities(native_device_facts{});
    CHECK(unsupported_caps.availability == metadata_availability::unavailable);
}

TEST_CASE("metadata.defaults_do_not_claim_battery_or_transport_facts", "[unit][metadata][policy]") {
    const metadata_snapshot snapshot{};
    CHECK(snapshot.battery_telemetry.availability == metadata_availability::unsupported);
    CHECK(snapshot.transport.availability == metadata_availability::unavailable);
    CHECK(snapshot.transport.value.value == physical_transport::unknown);
    CHECK(snapshot.transport.value.provenance == transport_provenance::unavailable);
}
