#include "xinput/query_projection.hpp"

#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace inputnext;

TEST_CASE("base and dynamic capabilities project only available facts", "[unit][xinput]") {
    const auto base = xinput::detail::base_capabilities();
    CHECK(base.Type == XINPUT_DEVTYPE_GAMEPAD);
    CHECK(base.SubType == XINPUT_DEVSUBTYPE_GAMEPAD);
    CHECK(base.Flags == 0U);
    CHECK(base.Gamepad.wButtons == 0xF3FFU);
    CHECK(base.Gamepad.bLeftTrigger == 255U);
    CHECK(base.Gamepad.bRightTrigger == 255U);
    CHECK(base.Gamepad.sThumbLX == -32768);
    CHECK(base.Gamepad.sThumbLY == -32768);
    CHECK(base.Gamepad.sThumbRX == -32768);
    CHECK(base.Gamepad.sThumbRY == -32768);
    CHECK(base.Vibration.wLeftMotorSpeed == 0U);
    CHECK(base.Vibration.wRightMotorSpeed == 0U);

    metadata_snapshot facts{};
    const auto unavailable = xinput::detail::dynamic_capabilities(facts);
    CHECK(unavailable.Flags == 0U);
    CHECK(unavailable.Vibration.wLeftMotorSpeed == 0U);
    CHECK(unavailable.Vibration.wRightMotorSpeed == 0U);

    facts.transport = {.availability = metadata_availability::available,
        .value = {.value = physical_transport::bluetooth,
            .provenance = transport_provenance::native_bus_type}};
    facts.capabilities = {.availability = metadata_availability::available,
        .value = {.gamepad = true, .low_frequency_rumble = false, .high_frequency_rumble = true}};
    facts.headset_render.id = {.availability = metadata_availability::available,
        .value = u"render-endpoint"};

    const auto dynamic = xinput::detail::dynamic_capabilities(facts);
    CHECK((dynamic.Flags & XINPUT_CAPS_WIRELESS) != 0U);
    CHECK((dynamic.Flags & XINPUT_CAPS_FFB_SUPPORTED) != 0U);
    CHECK((dynamic.Flags & XINPUT_CAPS_VOICE_SUPPORTED) != 0U);
    CHECK(dynamic.Vibration.wLeftMotorSpeed == 0U);
    CHECK(dynamic.Vibration.wRightMotorSpeed == 65535U);

    facts.transport.value.value = physical_transport::usb;
    const auto wired = xinput::detail::dynamic_capabilities(facts);
    CHECK((wired.Flags & XINPUT_CAPS_WIRELESS) == 0U);
}

TEST_CASE("dynamic capabilities project independent rumble motor support", "[unit][xinput]") {
    for (const bool low_frequency : {false, true}) {
        for (const bool high_frequency : {false, true}) {
            metadata_snapshot facts{};
            facts.capabilities = {.availability = metadata_availability::available,
                .value = {.gamepad = true,
                    .low_frequency_rumble = low_frequency,
                    .high_frequency_rumble = high_frequency}};

            const auto projected = xinput::detail::dynamic_capabilities(facts);
            CHECK(projected.Vibration.wLeftMotorSpeed == (low_frequency ? 65535U : 0U));
            CHECK(projected.Vibration.wRightMotorSpeed == (high_frequency ? 65535U : 0U));
            CHECK(((projected.Flags & XINPUT_CAPS_FFB_SUPPORTED) != 0U) ==
                  (low_frequency || high_frequency));
        }
    }
}

TEST_CASE("validated nonzero endpoint GUIDs establish voice association", "[unit][xinput]") {
    metadata_snapshot facts{};
    CHECK_FALSE(xinput::detail::has_voice_association(facts));

    facts.headset_render.id = {.availability = metadata_availability::available,
        .value = u"render"};
    CHECK(xinput::detail::has_voice_association(facts));

    facts.headset_render.id.availability = metadata_availability::unavailable;
    facts.headset_render.direct_sound_guid.availability = metadata_availability::available;
    facts.headset_render.direct_sound_guid.value.bytes[0U] = std::byte{0xA6U};
    CHECK(xinput::detail::has_voice_association(facts));

    facts.headset_render.direct_sound_guid.value = {};
    CHECK_FALSE(xinput::detail::has_voice_association(facts));

    facts.headset_render.direct_sound_guid.availability = metadata_availability::unavailable;
    facts.headset_render.direct_sound_guid.value.bytes[0U] = std::byte{0xA6U};
    CHECK_FALSE(xinput::detail::has_voice_association(facts));
}

TEST_CASE("extended capabilities expose only available native identifiers", "[unit][xinput]") {
    metadata_snapshot facts{};
    auto projected = xinput::detail::extended_capabilities(facts);
    CHECK(projected.vendor_id == 0U);
    CHECK(projected.product_id == 0U);
    CHECK(projected.product_version == 0U);
    CHECK(projected.unknown1 == 0U);

    facts.native.vendor_id = {.availability = metadata_availability::available, .value = 0x054CU};
    facts.native.product_id = {.availability = metadata_availability::available, .value = 0x0CE6U};
    facts.native.revision = {.availability = metadata_availability::available, .value = 0x0123U};
    projected = xinput::detail::extended_capabilities(facts);
    CHECK(projected.vendor_id == 0x054CU);
    CHECK(projected.product_id == 0x0CE6U);
    CHECK(projected.product_version == 0x0123U);
    CHECK(projected.unknown1 == 0U);
    CHECK(sizeof(projected) == 28U);
}

TEST_CASE("GameInput battery projection stays at truthful transport fallback", "[unit][xinput]") {
    metadata_snapshot facts{};
    auto battery = xinput::detail::gamepad_battery(facts);
    CHECK(battery.BatteryType == BATTERY_TYPE_UNKNOWN);
    CHECK(battery.BatteryLevel == BATTERY_LEVEL_EMPTY);

    facts.transport = {.availability = metadata_availability::available,
        .value = {.value = physical_transport::usb,
            .provenance = transport_provenance::native_bus_type}};
    battery = xinput::detail::gamepad_battery(facts);
    CHECK(battery.BatteryType == BATTERY_TYPE_WIRED);
    CHECK(battery.BatteryLevel == BATTERY_LEVEL_EMPTY);
}

TEST_CASE("audio endpoint sizing and ordered writes match the XInput contract", "[unit][xinput]") {
    const std::u16string endpoint = u"render";
    std::uint32_t flags = 0U;
    UINT required = 0U;
    CHECK(xinput::detail::write_endpoint(endpoint, nullptr, &required, 0x01U, 0x02U, &flags) ==
          xinput::detail::endpoint_write_result::success);
    CHECK(required == 7U);
    CHECK(flags == 0x01U);

    std::array<wchar_t, 3U> short_buffer{L'x', L'y', L'z'};
    UINT short_capacity = static_cast<UINT>(short_buffer.size());
    CHECK(xinput::detail::write_endpoint(endpoint,
              short_buffer.data(),
              &short_capacity,
              0x01U,
              0x02U,
              &flags) == xinput::detail::endpoint_write_result::insufficient_buffer);
    CHECK(short_capacity == 7U);
    CHECK((short_buffer == std::array<wchar_t, 3U>{L'x', L'y', L'z'}));
    CHECK(flags == 0x01U);

    std::array<wchar_t, 8U> render_buffer{};
    UINT render_capacity = static_cast<UINT>(render_buffer.size());
    CHECK(xinput::detail::write_endpoint(endpoint,
              render_buffer.data(),
              &render_capacity,
              0x01U,
              0x02U,
              &flags) == xinput::detail::endpoint_write_result::success);
    CHECK(render_capacity == 7U);
    CHECK(render_buffer[0] == L'r');
    CHECK(render_buffer[5] == L'r');
    CHECK(render_buffer[6] == L'\0');
    CHECK(flags == 0x03U);

    std::array<wchar_t, 2U> capture_buffer{L'a', L'b'};
    UINT capture_capacity = 1U;
    CHECK(xinput::detail::write_endpoint(u"capture",
              capture_buffer.data(),
              &capture_capacity,
              0x04U,
              0x08U,
              &flags) == xinput::detail::endpoint_write_result::insufficient_buffer);
    CHECK(capture_capacity == 8U);
    CHECK((capture_buffer == std::array<wchar_t, 2U>{L'a', L'b'}));
    CHECK(flags == 0x07U);

    UINT absent_count = 1U;
    wchar_t empty_buffer = L'x';
    CHECK(xinput::detail::write_endpoint({}, &empty_buffer, &absent_count, 0x04U, 0x08U, &flags) ==
          xinput::detail::endpoint_write_result::success);
    CHECK(empty_buffer == L'\0');
    CHECK(absent_count == 0U);
    CHECK(flags == 0x0FU);

    flags = 0U;
    wchar_t skipped_buffer = L's';
    CHECK(
        xinput::detail::write_endpoint(endpoint, &skipped_buffer, nullptr, 0x01U, 0x02U, &flags) ==
        xinput::detail::endpoint_write_result::success);
    CHECK(skipped_buffer == L's');
    CHECK(flags == 0U);
}

TEST_CASE("audio endpoint UTF-16 copying and capacity bound are explicit", "[unit][xinput]") {
    const std::u16string surrogate_pair{static_cast<char16_t>(0xD83DU),
        static_cast<char16_t>(0xDE00U)};
    std::array<wchar_t, 3U> output{};
    UINT capacity = static_cast<UINT>(output.size());
    CHECK(xinput::detail::write_endpoint(surrogate_pair,
              output.data(),
              &capacity,
              0x01U,
              0x02U,
              nullptr) == xinput::detail::endpoint_write_result::success);
    CHECK(static_cast<std::uint16_t>(output[0]) == 0xD83DU);
    CHECK(static_cast<std::uint16_t>(output[1]) == 0xDE00U);
    CHECK(output[2] == L'\0');

    metadata_snapshot facts{};
    facts.headset_render.id = {.availability = metadata_availability::available,
        .value = std::u16string(headset_endpoint_content_code_units, u'a')};
    const auto valid = xinput::detail::endpoint_id(facts.headset_render);
    CHECK(valid.size() == headset_endpoint_content_code_units);
    UINT required = 0U;
    CHECK(xinput::detail::write_endpoint(valid, nullptr, &required, 0x01U, 0x02U, nullptr) ==
          xinput::detail::endpoint_write_result::success);
    CHECK(required == headset_endpoint_buffer_code_units);

    facts.headset_render.id.value.push_back(u'b');
    CHECK(xinput::detail::endpoint_id(facts.headset_render).empty());
    CHECK(xinput::detail::has_voice_association(facts) == false);
}

TEST_CASE("DirectSound GUID projection preserves native bytes and absent facts are null",
    "[unit][xinput]") {
    metadata_field<native_guid> field{};
    const auto zero = xinput::detail::project_guid(field);
    CHECK(zero.Data1 == 0U);
    CHECK(zero.Data2 == 0U);
    CHECK(zero.Data3 == 0U);
    CHECK(std::ranges::all_of(zero.Data4, [](BYTE value) { return value == 0U; }));
    field.availability = metadata_availability::available;
    field.value.bytes = {std::byte{0x78},
        std::byte{0x56},
        std::byte{0x34},
        std::byte{0x12},
        std::byte{0xBC},
        std::byte{0x9A},
        std::byte{0xF0},
        std::byte{0xDE},
        std::byte{0x01},
        std::byte{0x23},
        std::byte{0x45},
        std::byte{0x67},
        std::byte{0x89},
        std::byte{0xAB},
        std::byte{0xCD},
        std::byte{0xEF}};
    const auto guid = xinput::detail::project_guid(field);
    CHECK(guid.Data1 == 0x12345678U);
    CHECK(guid.Data2 == 0x9ABCU);
    CHECK(guid.Data3 == 0xDEF0U);
    CHECK(guid.Data4[0] == 0x01U);
    CHECK(guid.Data4[7] == 0xEFU);
}
