#include "cpp/integration/dinput/input/support.hpp"
#include "runtime/abi/dinput/api.hpp"
#include "runtime/abi/xinput/api.hpp"
#include "runtime_api_test_composition.hpp"

#include <input_next_core.h>
#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <Xinput.h>
#include <dinput.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <thread>

namespace {

using inputnext::runtime_detail::runtime_api_test_fixture;
using namespace std::chrono_literals;

template <typename Interface>
struct release_interface final {
    void operator()(Interface* value) const noexcept {
        if (value != nullptr) {
            value->Release();
        }
    }
};

struct enumerated_inputnext_device final {
    bool found{};
    DIDEVICEINSTANCEW instance{};
};

BOOL CALLBACK find_inputnext_gamepad(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    auto& result = *static_cast<enumerated_inputnext_device*>(context);
    if (std::wcscmp(instance->tszInstanceName, L"InputNext Gamepad") == 0) {
        result.instance = *instance;
        result.found = true;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

[[nodiscard]] bool wait_for_device(const input_next_core_api_v1& core,
    input_next_device_target_v1& target,
    input_next_input_snapshot_v1& snapshot) {
    std::array<input_next_device_info_v1, 16U> devices{};
    for (auto& device : devices) {
        device.struct_size = static_cast<std::uint32_t>(sizeof(device));
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        std::uint32_t count{};
        std::uint64_t revision{};
        const auto enumerated = core.enumerate_devices(devices.data(),
            static_cast<std::uint32_t>(devices.size()),
            &count,
            &revision);
        if (enumerated == INPUT_NEXT_STATUS_OK && count == 1U && revision != 0U) {
            target = devices[0U].target;
            snapshot = {};
            snapshot.struct_size = static_cast<std::uint32_t>(sizeof(snapshot));
            if (core.read_latest(&target, &snapshot) == INPUT_NEXT_STATUS_OK &&
                snapshot.sequence != 0U) {
                return true;
            }
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

[[nodiscard]] bool wait_for_xinput(const input_next_runtime_api_v1& xinput, XINPUT_STATE& state) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        state = {};
        if (xinput.get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &state) == ERROR_SUCCESS) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

} // namespace

TEST_CASE("runtime.public_core_xinput_and_directinput_share_one_session_and_input",
    "[integration][runtime][directinput][coexistence]") {
    runtime_api_test_fixture fixture;
    const auto* const core = input_next_runtime_query_core_api(INPUT_NEXT_CORE_ABI_VERSION);
    const auto* const xinput = input_next_runtime_query_api(1U);
    const auto* const directinput = input_next_runtime_query_directinput_api(1U);
    REQUIRE(core != nullptr);
    REQUIRE(xinput != nullptr);
    REQUIRE(directinput != nullptr);
    REQUIRE(is_valid_input_next_directinput_api_v1(directinput));

    input_next_runtime_info_v1 runtime{};
    runtime.struct_size = static_cast<std::uint32_t>(sizeof(runtime));
    REQUIRE(core->get_runtime_info(&runtime) == INPUT_NEXT_STATUS_OK);
    REQUIRE(fixture.owner_is_ready());
    REQUIRE(fixture.prepare_attempts() == 1U);
    REQUIRE(fixture.owner_creations() == 1U);
    REQUIRE(fixture.native_initialized());

    fixture.connect_native_device();
    input_next_device_target_v1 target{};
    input_next_input_snapshot_v1 latest{};
    REQUIRE(wait_for_device(*core, target, latest));
    CHECK(target.runtime_id[0U] == runtime.runtime_id[0U]);
    CHECK(target.runtime_id[1U] == runtime.runtime_id[1U]);
    CHECK(latest.domain.runtime_id[0U] == target.runtime_id[0U]);
    CHECK(latest.domain.runtime_id[1U] == target.runtime_id[1U]);
    CHECK(latest.domain.device_key == target.device_key);
    CHECK(latest.domain.connection_id == target.connection_id);
    CHECK((latest.state.buttons & static_cast<std::uint32_t>(inputnext::gamepad_button::south)) !=
          0U);
    CHECK(latest.state.left_x == 0.25F);
    CHECK(latest.state.left_y == -0.25F);
    CHECK(latest.state.right_x == -0.25F);
    CHECK(latest.state.right_y == 0.25F);
    CHECK(latest.state.left_trigger == 0.25F);
    CHECK(latest.state.right_trigger == 0.75F);

    XINPUT_STATE xinput_state{};
    REQUIRE(wait_for_xinput(*xinput, xinput_state));
    CHECK((xinput_state.Gamepad.wButtons & static_cast<WORD>(XINPUT_GAMEPAD_A)) != 0U);
    CHECK(xinput_state.Gamepad.sThumbLX == 8191);
    CHECK(xinput_state.Gamepad.sThumbLY == -8192);
    CHECK(xinput_state.Gamepad.sThumbRX == -8192);
    CHECK(xinput_state.Gamepad.sThumbRY == 8191);
    CHECK(xinput_state.Gamepad.bLeftTrigger == 63U);
    CHECK(xinput_state.Gamepad.bRightTrigger == 191U);

    void* root_value{};
    const auto root_status = directinput->create(GetModuleHandleW(nullptr),
        0x0800U,
        IID_IDirectInput8W,
        &root_value,
        nullptr);
    std::unique_ptr<IDirectInput8W, release_interface<IDirectInput8W>> root{
        static_cast<IDirectInput8W*>(root_value)};
    REQUIRE(SUCCEEDED(root_status));
    REQUIRE(root != nullptr);

    enumerated_inputnext_device selected{};
    REQUIRE(root->EnumDevices(DI8DEVCLASS_GAMECTRL,
                &find_inputnext_gamepad,
                &selected,
                DIEDFL_ATTACHEDONLY) == DI_OK);
    REQUIRE(selected.found);
    CHECK(selected.instance.dwSize == static_cast<DWORD>(sizeof(DIDEVICEINSTANCEW)));

    IDirectInputDevice8W* raw_device{};
    const auto create_status =
        root->CreateDevice(selected.instance.guidInstance, &raw_device, nullptr);
    std::unique_ptr<IDirectInputDevice8W, release_interface<IDirectInputDevice8W>> device{
        raw_device};
    REQUIRE(SUCCEEDED(create_status));
    REQUIRE(device != nullptr);

    const auto* const joystick_format = directinput->joystick_format();
    REQUIRE(joystick_format != nullptr);
    REQUIRE(device->SetDataFormat(joystick_format) == DI_OK);
    const inputnext::dinput_test_support::hidden_window window;
    REQUIRE(
        device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    DIJOYSTATE directinput_state{};
    REQUIRE(device->GetDeviceState(static_cast<DWORD>(sizeof(directinput_state)),
                &directinput_state) == DI_OK);
    CHECK(directinput_state.lX == 40959);
    CHECK(directinput_state.lY == 40959);
    CHECK(directinput_state.lZ == 16384);
    CHECK(directinput_state.lRx == 24575);
    CHECK(directinput_state.lRy == 24575);
    CHECK(directinput_state.lRz == 49151);
    CHECK(directinput_state.rgbButtons[0U] == 0x80U);

    xinput->enable(INPUT_NEXT_FLAVOR_1_4, FALSE);
    XINPUT_STATE disabled_xinput_state{};
    REQUIRE(xinput->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &disabled_xinput_state) == ERROR_SUCCESS);
    CHECK(disabled_xinput_state.Gamepad.wButtons == 0U);
    CHECK(disabled_xinput_state.Gamepad.sThumbLX == 0);
    CHECK(disabled_xinput_state.Gamepad.sThumbLY == 0);
    CHECK(disabled_xinput_state.Gamepad.sThumbRX == 0);
    CHECK(disabled_xinput_state.Gamepad.sThumbRY == 0);
    CHECK(disabled_xinput_state.Gamepad.bLeftTrigger == 0U);
    CHECK(disabled_xinput_state.Gamepad.bRightTrigger == 0U);

    latest = {};
    latest.struct_size = static_cast<std::uint32_t>(sizeof(latest));
    REQUIRE(core->read_latest(&target, &latest) == INPUT_NEXT_STATUS_OK);
    CHECK((latest.state.buttons & static_cast<std::uint32_t>(inputnext::gamepad_button::south)) !=
          0U);
    directinput_state = {};
    REQUIRE(device->GetDeviceState(static_cast<DWORD>(sizeof(directinput_state)),
                &directinput_state) == DI_OK);
    CHECK(directinput_state.rgbButtons[0U] == 0x80U);
    CHECK(fixture.prepare_attempts() == 1U);
    CHECK(fixture.owner_creations() == 1U);
}
