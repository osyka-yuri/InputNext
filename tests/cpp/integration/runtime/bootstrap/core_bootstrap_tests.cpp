#include "runtime_api_test_composition.hpp"

#include "runtime/abi/runtime_api.hpp"

#include <input_next_core.h>
#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using inputnext::runtime_detail::runtime_api_test_fixture;

[[nodiscard]] input_next_runtime_info_v1 runtime_info(const input_next_core_api_v1& api) {
    input_next_runtime_info_v1 info{};
    info.struct_size = sizeof(info);
    REQUIRE(api.get_runtime_info(&info) == INPUT_NEXT_STATUS_OK);
    return info;
}

[[nodiscard]] input_next_device_target_v1 wait_for_device(const input_next_core_api_v1& api) {
    std::array<input_next_device_info_v1, 16U> devices{};
    for (auto& device : devices) {
        device.struct_size = sizeof(device);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::uint32_t count{};
    std::uint64_t revision{};
    do {
        if (api.enumerate_devices(devices.data(),
                static_cast<std::uint32_t>(devices.size()),
                &count,
                &revision) == INPUT_NEXT_STATUS_OK &&
            count == 1U) {
            CHECK(revision != 0U);
            return devices[0U].target;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("Controlled Core device was not discovered");
}

} // namespace

TEST_CASE("runtime.core.bootstrap_queries_share_owner_without_native_startup",
    "[integration][runtime][core][bootstrap]") {
    runtime_api_test_fixture fixture;
    CHECK(input_next_runtime_query_core_api(0U) == nullptr);
    CHECK(fixture.pin_calls() == 0U);
    CHECK(fixture.owner_creations() == 0U);

    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(core != nullptr);
    CHECK(input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION) == core);
    REQUIRE(input_next_runtime_query_api(1U) != nullptr);
    CHECK(fixture.pin_calls() == 1U);
    CHECK(fixture.owner_creations() == 1U);
    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());

    input_next_device_target_v1 target{};
    target.struct_size = sizeof(target);
    target.runtime_id[0U] = 1U;
    target.device_key = 1U;
    target.connection_id = 1U;
    input_next_input_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    CHECK(core->read_latest(&target, &snapshot) == INPUT_NEXT_STATUS_UNAVAILABLE);
    CHECK(fixture.prepare_attempts() == 0U);
}

TEST_CASE("runtime.core.bootstrap_control_calls_share_the_xinput_session",
    "[integration][runtime][core][bootstrap]") {
    runtime_api_test_fixture fixture;
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    const auto* const xinput = input_next_runtime_query_api(1U);
    REQUIRE(core != nullptr);
    REQUIRE(xinput != nullptr);

    SECTION("Core starts the shared session") {
        const auto info = runtime_info(*core);
        CHECK(info.max_devices == 16U);
        CHECK(info.max_readers == 28U);
        CHECK(info.max_effect_contexts == 32U);
        CHECK(info.qpc_frequency != 0U);
        CHECK((info.runtime_id[0U] != 0U || info.runtime_id[1U] != 0U));
    }
    SECTION("XInput starts the shared session") {
        XINPUT_STATE state{};
        const DWORD state_status = xinput->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &state);
        CHECK(state_status == ERROR_DEVICE_NOT_CONNECTED);
    }
    REQUIRE(fixture.owner_is_ready());
    REQUIRE(fixture.prepare_attempts() == 1U);
    fixture.connect_native_device();
    const auto target = wait_for_device(*core);
    const auto info = runtime_info(*core);
    CHECK(target.runtime_id[0U] == info.runtime_id[0U]);
    CHECK(target.runtime_id[1U] == info.runtime_id[1U]);

    input_next_input_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    xinput->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    REQUIRE(core->read_latest(&target, &snapshot) == INPUT_NEXT_STATUS_OK);
    CHECK(snapshot.domain.stream_id == 0U);
    CHECK(snapshot.sequence != 0U);
    CHECK((snapshot.state.buttons & static_cast<std::uint32_t>(inputnext::gamepad_button::south)) !=
          0U);
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.owner_creations() == 1U);
}

TEST_CASE("runtime.core.bootstrap_degraded_session_keeps_runtime_information",
    "[integration][runtime][core][bootstrap]") {
    runtime_api_test_fixture fixture{true};
    const auto* const api = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    REQUIRE(api != nullptr);
    const auto first = runtime_info(*api);
    REQUIRE(fixture.owner_is_degraded());
    const auto second = runtime_info(*api);
    CHECK(first.runtime_id[0U] == second.runtime_id[0U]);
    CHECK(first.runtime_id[1U] == second.runtime_id[1U]);
    CHECK(first.qpc_frequency == second.qpc_frequency);
    CHECK(fixture.prepare_attempts() == 1U);
    std::uint32_t count{};
    std::uint64_t revision{};
    CHECK(api->enumerate_devices(nullptr, 0U, &count, &revision) == INPUT_NEXT_STATUS_UNAVAILABLE);
}
