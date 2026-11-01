#include "cpp/integration/runtime/bootstrap/runtime_api_test_composition.hpp"

#include <core/core.hpp>
#include <input_next_core.h>
#include <inputnext/gamepad_state.hpp>
#include <runtime/abi/xinput/api.hpp>

#include <catch2/catch_test_macros.hpp>
#include <values/required_value.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

namespace {

using inputnext::runtime_detail::runtime_api_test_fixture;
using inputnext::test::required_value;

[[nodiscard]] std::optional<input_next_device_target_v1> wait_for_device(
    const input_next_core_api_v1& api) {
    std::array<input_next_device_info_v1, 16U> devices{};
    for (auto& device : devices) {
        device.struct_size = sizeof(device);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        std::uint32_t count{};
        std::uint64_t revision{};
        if (api.enumerate_devices(devices.data(),
                static_cast<std::uint32_t>(devices.size()),
                &count,
                &revision) == INPUT_NEXT_STATUS_OK &&
            count == 1U && revision != 0U) {
            return devices[0U].target;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);

    return std::nullopt;
}

[[nodiscard]] input_next_status open_reader_when_ready(const input_next_core_api_v1& api,
    const input_next_reader_request_v1& request,
    input_next_reader_v1& reader) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto status = api.open_reader(&request, &reader);
    while (status == INPUT_NEXT_STATUS_BUSY && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        status = api.open_reader(&request, &reader);
    }
    return status;
}

[[nodiscard]] input_next_status read_events_when_ready(const input_next_core_api_v1& api,
    const input_next_reader_v1& reader,
    std::array<input_next_input_snapshot_v1, 32U>& snapshots,
    input_next_reader_result_v1& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto status = api.read_events(&reader,
        snapshots.data(),
        static_cast<std::uint32_t>(snapshots.size()),
        &result);
    while (status == INPUT_NEXT_STATUS_BUSY && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        status = api.read_events(&reader,
            snapshots.data(),
            static_cast<std::uint32_t>(snapshots.size()),
            &result);
    }
    return status;
}

} // namespace

TEST_CASE("runtime.core.abi_rejects_malformed_inputs_before_bootstrap_or_output_changes",
    "[integration][runtime][core][abi]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = 0U;
    info.qpc_frequency = 0x1234U;
    CHECK(api->get_runtime_info(&info) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(info.qpc_frequency == 0x1234U);

    info.struct_size = sizeof(info);
    info.flags = 1U;
    CHECK(api->get_runtime_info(&info) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(info.qpc_frequency == 0x1234U);

    input_next_device_selector_v1 selector{};
    selector.struct_size = sizeof(selector);
    selector.kind = INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT;
    selector.xinput_slot = 4U;
    selector.target.struct_size = sizeof(selector.target);
    input_next_device_target_v1 target{};
    target.struct_size = sizeof(target);
    target.runtime_id[0U] = 0x1234U;
    target.device_key = 0x5678U;
    CHECK(api->resolve_target(&selector, &target) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(target.runtime_id[0U] == 0x1234U);
    CHECK(target.device_key == 0x5678U);

    std::array<input_next_device_info_v1, 2U> devices{};
    devices[0U].struct_size = sizeof(devices[0U]);
    std::uint32_t count = 0xA5U;
    std::uint64_t revision = 0xB6U;
    CHECK(api->enumerate_devices(devices.data(),
              static_cast<std::uint32_t>(devices.size()),
              &count,
              &revision) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(count == 0xA5U);
    CHECK(revision == 0xB6U);

    CHECK(api->enumerate_devices(nullptr,
              static_cast<std::uint32_t>(inputnext::core::device_capacity + 1U),
              &count,
              &revision) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(count == 0xA5U);
    CHECK(revision == 0xB6U);

    input_next_reader_v1 reader{};
    reader.struct_size = sizeof(reader);
    reader.runtime_id[0U] = 1U;
    reader.id = 1U;
    input_next_reader_result_v1 result{};
    result.struct_size = sizeof(result);
    result.flags = 0xC7U;
    CHECK(api->read_events(&reader, nullptr, 0U, &result) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(result.flags == 0xC7U);

    std::array<input_next_input_snapshot_v1, 2U> snapshots{};
    snapshots[0U].struct_size = sizeof(snapshots[0U]);
    result.flags = 0xD8U;
    CHECK(api->read_events(&reader,
              snapshots.data(),
              static_cast<std::uint32_t>(snapshots.size()),
              &result) == INPUT_NEXT_STATUS_INVALID_ARGUMENT);
    CHECK(result.flags == 0xD8U);

    input_next_device_target_v1 valid_target{};
    valid_target.struct_size = sizeof(valid_target);
    valid_target.runtime_id[0U] = 1U;
    valid_target.device_key = 1U;
    valid_target.connection_id = 1U;
    input_next_input_snapshot_v1 latest{};
    latest.struct_size = sizeof(latest);
    latest.flags = 0xE9U;
    CHECK(api->read_latest(&valid_target, &latest) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(latest.flags == 0U);

    snapshots[0U].flags = 0xFAU;
    snapshots[1U].struct_size = sizeof(snapshots[1U]);
    result.flags = 0xABU;
    CHECK(
        api->read_events(&reader, snapshots.data(), 1U, &result) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(result.flags == 0U);
    CHECK(snapshots[0U].flags == 0xFAU);

    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());
}

TEST_CASE("runtime.core.enumeration_accepts_capacity_above_the_device_limit",
    "[integration][runtime][core][abi]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(api->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);

    std::array<input_next_device_info_v1, 32U> devices{};
    for (auto& device : devices) {
        device.struct_size = sizeof(device);
    }
    std::uint32_t count{};
    std::uint64_t revision{};
    CHECK(api->enumerate_devices(devices.data(), 32U, &count, &revision) == INPUT_NEXT_STATUS_OK);
    CHECK(count == 0U);
    CHECK(revision != 0U);
}

TEST_CASE("runtime.core.abi_projects_targets_latest_and_reader_history",
    "[integration][runtime][core][abi]") {
    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    const auto* const xinput = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    REQUIRE(xinput != nullptr);

    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(api->get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    fixture.connect_native_device();

    const auto target = required_value(wait_for_device(*api));

    input_next_device_selector_v1 exact_selector{};
    exact_selector.struct_size = sizeof(exact_selector);
    exact_selector.kind = INPUT_NEXT_CORE_SELECTOR_EXACT_TARGET;
    exact_selector.target = target;
    input_next_device_target_v1 resolved{};
    resolved.struct_size = sizeof(resolved);
    REQUIRE(api->resolve_target(&exact_selector, &resolved) == INPUT_NEXT_STATUS_OK);
    CHECK(resolved.runtime_id[0U] == target.runtime_id[0U]);
    CHECK(resolved.runtime_id[1U] == target.runtime_id[1U]);
    CHECK(resolved.device_key == target.device_key);
    CHECK(resolved.connection_id == target.connection_id);

    XINPUT_STATE xinput_state{};
    auto xinput_status = static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);
    const auto xinput_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    do {
        xinput_status = xinput->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &xinput_state);
        if (xinput_status == ERROR_SUCCESS) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < xinput_deadline);
    REQUIRE(xinput_status == ERROR_SUCCESS);

    input_next_device_selector_v1 slot_selector{};
    slot_selector.struct_size = sizeof(slot_selector);
    slot_selector.kind = INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT;
    slot_selector.target.struct_size = sizeof(slot_selector.target);
    input_next_device_target_v1 slot_target{};
    slot_target.struct_size = sizeof(slot_target);
    REQUIRE(api->resolve_target(&slot_selector, &slot_target) == INPUT_NEXT_STATUS_OK);
    CHECK(slot_target.runtime_id[0U] == target.runtime_id[0U]);
    CHECK(slot_target.runtime_id[1U] == target.runtime_id[1U]);
    CHECK(slot_target.device_key == target.device_key);
    CHECK(slot_target.connection_id == target.connection_id);

    input_next_input_snapshot_v1 latest{};
    latest.struct_size = sizeof(latest);
    REQUIRE(api->read_latest(&resolved, &latest) == INPUT_NEXT_STATUS_OK);
    CHECK(latest.domain.stream_id == 0U);
    CHECK(latest.sequence != 0U);
    CHECK((latest.state.buttons & static_cast<std::uint32_t>(inputnext::gamepad_button::south)) !=
          0U);

    input_next_reader_request_v1 request{};
    request.struct_size = sizeof(request);
    request.focus_mode = INPUT_NEXT_CORE_READER_FOREGROUND_PROCESS;
    request.target = resolved;
    input_next_reader_v1 reader{};
    reader.struct_size = sizeof(reader);
    REQUIRE(open_reader_when_ready(*api, request, reader) == INPUT_NEXT_STATUS_OK);
    CHECK(reader.id != 0U);
    CHECK(reader.runtime_id[0U] == info.runtime_id[0U]);
    CHECK(reader.runtime_id[1U] == info.runtime_id[1U]);

    std::array<input_next_input_snapshot_v1, 32U> snapshots{};
    for (auto& snapshot : snapshots) {
        snapshot.struct_size = sizeof(snapshot);
    }
    snapshots[0U].flags = 0xF0U;
    input_next_reader_result_v1 result{};
    result.struct_size = sizeof(result);
    const auto read_status = read_events_when_ready(*api, reader, snapshots, result);
    REQUIRE(read_status == INPUT_NEXT_STATUS_OK);
    REQUIRE(result.count == 1U);
    CHECK((result.flags & INPUT_NEXT_CORE_READER_RESULT_BASELINE) != 0U);
    CHECK(result.reset_reason == INPUT_NEXT_CORE_READER_RESET_INITIAL_OPEN);
    CHECK(result.domain.stream_id != 0U);
    CHECK(snapshots[0U].domain.stream_id == result.domain.stream_id);
    CHECK((snapshots[0U].flags & INPUT_NEXT_CORE_SNAPSHOT_BASELINE) != 0U);
    CHECK(snapshots[0U].sequence != 0U);

    const auto followup_status = read_events_when_ready(*api, reader, snapshots, result);
    CHECK(
        (followup_status == INPUT_NEXT_STATUS_OK || followup_status == INPUT_NEXT_STATUS_NO_DATA));

    CHECK(api->close_reader(&reader) == INPUT_NEXT_STATUS_OK);
    CHECK(api->close_reader(&reader) == INPUT_NEXT_STATUS_STALE_READER);
}
