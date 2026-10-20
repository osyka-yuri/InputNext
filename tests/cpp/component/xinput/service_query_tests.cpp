#include "service_test_helpers.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <optional>
#include <thread>
#include <utility>

using namespace inputnext;
using namespace inputnext::xinput_test_support;

TEST_CASE("query validation, fixed projections, failures, and focus independence are preserved",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);

    XINPUT_CAPABILITIES capabilities{};
    CHECK(service.get_capabilities(xinput::flavor::v1_4, 0U, 2U, &capabilities) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(metadata.calls() == 0U);
    CHECK(service.get_capabilities(xinput::flavor::v1_4, 4U, 0U, &capabilities) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(service.get_capabilities(xinput::flavor::v1_4, 0U, 0U, nullptr) == ERROR_BAD_ARGUMENTS);
    CHECK(metadata.calls() == 0U);

    REQUIRE(
        service.get_capabilities(xinput::flavor::v9_1_0, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK(capabilities.Flags == 0U);
    CHECK(capabilities.Vibration.wLeftMotorSpeed == 65535U);
    CHECK(capabilities.Vibration.wRightMotorSpeed == 65535U);
    CHECK(metadata.calls() == 0U);

    XINPUT_BATTERY_INFORMATION battery{};
    REQUIRE(service.get_battery_information(xinput::flavor::v1_4,
                occupant.user_index,
                BATTERY_DEVTYPE_HEADSET,
                &battery) == ERROR_SUCCESS);
    CHECK(battery.BatteryType == BATTERY_TYPE_DISCONNECTED);
    CHECK(battery.BatteryLevel == BATTERY_LEVEL_EMPTY);
    CHECK(metadata.calls() == 0U);
    CHECK(service
              .get_audio_device_ids(xinput::flavor::v1_3, 4U, nullptr, nullptr, nullptr, nullptr) ==
          ERROR_NOT_SUPPORTED);
    CHECK(service.get_dsound_audio_device_guids(xinput::flavor::v1_4, 4U, nullptr, nullptr) ==
          ERROR_NOT_SUPPORTED);
    CHECK(metadata.calls() == 0U);
    REQUIRE(service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
                occupant.user_index,
                nullptr,
                nullptr) == ERROR_SUCCESS);
    CHECK(metadata.calls() == 0U);

    capabilities.Type = 0xAAU;
    metadata.set_failure(metadata_error::busy);
    CHECK(service.get_capabilities(xinput::flavor::v1_4, occupant.user_index, 0U, &capabilities) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(capabilities.Type == 0xAAU);

    metadata.set_failure(std::nullopt);
    metadata.mismatch_target(true);
    CHECK(service.get_capabilities(xinput::flavor::v1_4, occupant.user_index, 0U, &capabilities) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(capabilities.Type == 0xAAU);
    metadata.mismatch_target(false);

    auto available = metadata_for();
    available.native.vendor_id = {.availability = metadata_availability::available,
        .value = 0x054CU};
    available.native.product_id = {.availability = metadata_availability::available,
        .value = 0x0CE6U};
    available.native.revision = {.availability = metadata_availability::available,
        .value = 0x0123U};
    available.transport = {.availability = metadata_availability::available,
        .value = {.value = physical_transport::bluetooth,
            .provenance = transport_provenance::native_bus_type}};
    available.capabilities = {.availability = metadata_availability::available,
        .value = {.gamepad = true, .low_frequency_rumble = true}};
    available.headset_render.id = {.availability = metadata_availability::available,
        .value = u"render"};
    available.headset_render.direct_sound_guid.availability = metadata_availability::available;
    available.headset_render.direct_sound_guid.value.bytes[0] = std::byte{0x78};
    metadata.set_snapshot(std::move(available));

    REQUIRE(
        service.get_capabilities(xinput::flavor::v1_3, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK((capabilities.Flags & XINPUT_CAPS_WIRELESS) != 0U);
    CHECK((capabilities.Flags & XINPUT_CAPS_FFB_SUPPORTED) != 0U);
    CHECK((capabilities.Flags & XINPUT_CAPS_VOICE_SUPPORTED) != 0U);

    xinput::capabilities_ex capabilities_extended{};
    REQUIRE(service.get_capabilities_ex(xinput::flavor::v9_1_0,
                occupant.user_index,
                XINPUT_FLAG_GAMEPAD,
                &capabilities_extended) == ERROR_SUCCESS);
    CHECK(capabilities_extended.vendor_id == 0x054CU);
    CHECK(capabilities_extended.product_id == 0x0CE6U);
    CHECK(capabilities_extended.product_version == 0x0123U);
    CHECK(capabilities_extended.unknown1 == 0U);
    CHECK((capabilities_extended.capabilities.Flags & XINPUT_CAPS_WIRELESS) != 0U);

    std::array<wchar_t, 8U> render_id{};
    wchar_t capture_id = L'x';
    UINT render_count = static_cast<UINT>(render_id.size());
    UINT capture_count = 1U;
    std::uint32_t write_flags = 0U;
    REQUIRE(service.get_audio_device_ids(xinput::flavor::v1_4,
                occupant.user_index,
                render_id.data(),
                &render_count,
                &capture_id,
                &capture_count,
                &write_flags) == ERROR_SUCCESS);
    CHECK(render_id[0] == L'r');
    CHECK(render_id[5] == L'r');
    CHECK(render_id[6] == L'\0');
    CHECK(render_count == 7U);
    CHECK(capture_id == L'\0');
    CHECK(capture_count == 0U);
    CHECK(write_flags == 0x0FU);

    GUID render_guid{};
    GUID capture_guid{};
    REQUIRE(service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
                occupant.user_index,
                &render_guid,
                &capture_guid) == ERROR_SUCCESS);
    CHECK(render_guid.Data1 == 0x78U);
    CHECK(capture_guid.Data1 == 0U);

    focus_state.set(focus::evidence::background);
    service.enable(FALSE);
    REQUIRE(
        service.get_capabilities(xinput::flavor::v1_4, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK(capabilities.Type == XINPUT_DEVTYPE_GAMEPAD);
    XINPUT_STATE state{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, occupant.user_index, &state) == ERROR_SUCCESS);
    CHECK(state.Gamepad.wButtons == 0U);
}

TEST_CASE("validated GUID-only headset metadata projects independently and supports voice",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant = assign(service, connection);

    metadata_snapshot render_guid_only = metadata_for();
    render_guid_only.headset_render.id.availability = metadata_availability::not_present;
    render_guid_only.headset_render.direct_sound_guid.availability =
        metadata_availability::available;
    render_guid_only.headset_render.direct_sound_guid.value.bytes[0U] = std::byte{0xA6U};
    render_guid_only.headset_render.direct_sound_guid.value.bytes[1U] = std::byte{0x31U};
    render_guid_only.headset_capture.id.availability = metadata_availability::unsupported;
    render_guid_only.headset_capture.direct_sound_guid.availability =
        metadata_availability::unavailable;
    render_guid_only.headset_capture.direct_sound_guid.value.bytes[0U] = std::byte{0x5DU};
    metadata.set_snapshot(render_guid_only);

    XINPUT_CAPABILITIES capabilities{};
    REQUIRE(
        service.get_capabilities(xinput::flavor::v1_3, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK((capabilities.Flags & XINPUT_CAPS_VOICE_SUPPORTED) != 0U);

    xinput::capabilities_ex extended{};
    REQUIRE(service.get_capabilities_ex(xinput::flavor::v1_4, occupant.user_index, 0U, &extended) ==
            ERROR_SUCCESS);
    CHECK((extended.capabilities.Flags & XINPUT_CAPS_VOICE_SUPPORTED) != 0U);

    std::array<wchar_t, 2U> render_id{L'x', L'x'};
    std::array<wchar_t, 2U> capture_id{L'x', L'x'};
    UINT render_count = static_cast<UINT>(render_id.size());
    UINT capture_count = static_cast<UINT>(capture_id.size());
    REQUIRE(service.get_audio_device_ids(xinput::flavor::v1_4,
                occupant.user_index,
                render_id.data(),
                &render_count,
                capture_id.data(),
                &capture_count) == ERROR_SUCCESS);
    CHECK(render_count == 0U);
    CHECK(capture_count == 0U);
    CHECK(render_id[0U] == L'\0');
    CHECK(capture_id[0U] == L'\0');

    GUID render{};
    GUID capture{};
    REQUIRE(service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
                occupant.user_index,
                &render,
                &capture) == ERROR_SUCCESS);
    CHECK(render.Data1 == 0x31A6U);
    CHECK(capture.Data1 == 0U);

    metadata_snapshot capture_guid_only = metadata_for();
    capture_guid_only.headset_render.direct_sound_guid.availability =
        metadata_availability::unsupported;
    capture_guid_only.headset_render.direct_sound_guid.value.bytes[0U] = std::byte{0x11U};
    capture_guid_only.headset_capture.direct_sound_guid.availability =
        metadata_availability::available;
    capture_guid_only.headset_capture.direct_sound_guid.value.bytes[0U] = std::byte{0x5DU};
    capture_guid_only.headset_capture.direct_sound_guid.value.bytes[1U] = std::byte{0x72U};
    metadata.set_snapshot(capture_guid_only);

    REQUIRE(
        service.get_capabilities(xinput::flavor::v1_4, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK((capabilities.Flags & XINPUT_CAPS_VOICE_SUPPORTED) != 0U);
    render = {};
    capture = {};
    REQUIRE(service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
                occupant.user_index,
                &render,
                &capture) == ERROR_SUCCESS);
    CHECK(render.Data1 == 0U);
    CHECK(capture.Data1 == 0x725DU);

    metadata_snapshot zero_guids = metadata_for();
    zero_guids.headset_render.direct_sound_guid.availability = metadata_availability::available;
    zero_guids.headset_capture.direct_sound_guid.availability = metadata_availability::available;
    metadata.set_snapshot(zero_guids);
    REQUIRE(
        service.get_capabilities(xinput::flavor::v1_3, occupant.user_index, 0U, &capabilities) ==
        ERROR_SUCCESS);
    CHECK((capabilities.Flags & XINPUT_CAPS_VOICE_SUPPORTED) == 0U);
    render = {};
    capture = {};
    REQUIRE(service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
                occupant.user_index,
                &render,
                &capture) == ERROR_SUCCESS);
    CHECK(render.Data1 == 0U);
    CHECK(capture.Data1 == 0U);

    GUID unsupported_render{};
    unsupported_render.Data1 = 0xDEADBEEFU;
    GUID unsupported_capture{};
    unsupported_capture.Data1 = 0x12345678U;
    CHECK(service.get_dsound_audio_device_guids(xinput::flavor::v1_4,
              occupant.user_index,
              &unsupported_render,
              &unsupported_capture) == ERROR_NOT_SUPPORTED);
    CHECK(unsupported_render.Data1 == 0xDEADBEEFU);
    CHECK(unsupported_capture.Data1 == 0x12345678U);

    class retiring_metadata_source final : public xinput::metadata_source {
    public:
        explicit retiring_metadata_source(device_registry& registry) noexcept
            : registry_(registry) {}

        [[nodiscard]] metadata_result<metadata_snapshot> query(connection_target target) override {
            metadata_snapshot result{};
            result.target = target;
            result.headset_render.direct_sound_guid.availability = metadata_availability::available;
            result.headset_render.direct_sound_guid.value.bytes[0U] = std::byte{0xA6U};
            const auto retired = registry_.retire(target);
            if (!retired.has_value()) {
                return std::unexpected{metadata_error::not_connected};
            }
            return result;
        }

    private:
        device_registry& registry_;
    };

    registry_fixture retiring_devices;
    const auto retiring_connection = retiring_devices.add();
    retiring_metadata_source retiring_metadata{retiring_devices.registry()};
    xinput::service retiring_service{focus_state.observer(), retiring_metadata};
    const auto retiring_assignment = assign(retiring_service, retiring_connection);
    render = {};
    render.Data1 = 0xBADC0FFEU;
    capture = {};
    capture.Data1 = 0xCAFEBABEU;
    CHECK(retiring_service.get_dsound_audio_device_guids(xinput::flavor::v1_3,
              retiring_assignment.user_index,
              &render,
              &capture) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(render.Data1 == 0xBADC0FFEU);
    CHECK(capture.Data1 == 0xCAFEBABEU);
}

TEST_CASE("audio query keeps render-before-capture partial writes and metadata calls off the gate",
    "[component][xinput]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto first =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto second =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::east)});
    const auto first_assignment = assign(service, first);
    const auto second_assignment = assign(service, second);

    auto snapshot = metadata_for();
    snapshot.headset_render.id = {.availability = metadata_availability::available,
        .value = u"render"};
    snapshot.headset_capture.id = {.availability = metadata_availability::available,
        .value = u"capture"};
    metadata.set_snapshot(std::move(snapshot));

    std::array<wchar_t, 2U> render_short{L'r', L'x'};
    std::array<wchar_t, 8U> capture_buffer{};
    UINT render_count = static_cast<UINT>(render_short.size());
    UINT capture_count = static_cast<UINT>(capture_buffer.size());
    std::uint32_t flags = 0xFFFFFFFFU;
    CHECK(service.get_audio_device_ids(xinput::flavor::v1_4,
              first_assignment.user_index,
              render_short.data(),
              &render_count,
              capture_buffer.data(),
              &capture_count,
              &flags) == ERROR_INSUFFICIENT_BUFFER);
    CHECK(render_count == 7U);
    CHECK((render_short == std::array<wchar_t, 2U>{L'r', L'x'}));
    CHECK(capture_count == static_cast<UINT>(capture_buffer.size()));
    CHECK(capture_buffer[0] == L'\0');
    CHECK(flags == 0x01U);

    std::array<wchar_t, 8U> render_buffer{};
    render_count = static_cast<UINT>(render_buffer.size());
    capture_count = 1U;
    capture_buffer.fill(L'c');
    flags = 0xFFFFFFFFU;
    CHECK(service.get_audio_device_ids(xinput::flavor::v1_4,
              first_assignment.user_index,
              render_buffer.data(),
              &render_count,
              capture_buffer.data(),
              &capture_count,
              &flags) == ERROR_INSUFFICIENT_BUFFER);
    CHECK(render_count == 7U);
    CHECK(render_buffer[0] == L'r');
    CHECK(render_buffer[5] == L'r');
    CHECK(render_buffer[6] == L'\0');
    CHECK(capture_count == 8U);
    CHECK((
        capture_buffer == std::array<wchar_t, 8U>{L'c', L'c', L'c', L'c', L'c', L'c', L'c', L'c'}));
    CHECK(flags == 0x07U);

    metadata.block_queries();
    std::promise<DWORD> query_result;
    auto query_future = query_result.get_future();
    std::jthread query_worker([&] {
        XINPUT_CAPABILITIES result{};
        query_result.set_value(service.get_capabilities(xinput::flavor::v1_4,
            first_assignment.user_index,
            0U,
            &result));
    });
    const bool query_entered = metadata.wait_until_entered(std::chrono::seconds{2});

    std::promise<DWORD> read_result;
    auto read_future = read_result.get_future();
    std::jthread read_worker([&] {
        XINPUT_STATE result{};
        read_result.set_value(
            service.get_state(xinput::flavor::v1_4, second_assignment.user_index, &result));
    });
    const release_queries_on_exit release_queries{metadata};
    const bool read_completed_while_query_blocked =
        read_future.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready;
    metadata.release_queries();
    query_worker.join();
    read_worker.join();
    const auto query_status = query_future.get();
    const auto read_status = read_future.get();

    CHECK(query_entered);
    CHECK(read_completed_while_query_blocked);
    CHECK(query_status == ERROR_SUCCESS);
    CHECK(read_status == ERROR_SUCCESS);
}
