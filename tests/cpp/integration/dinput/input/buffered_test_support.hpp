#pragma once

#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/property_abi.h"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

namespace inputnext::dinput_test_support {

inline constexpr std::uint64_t buffered_cut_timestamp = 1'000U;
inline constexpr button_mask buffered_button = static_cast<button_mask>(gamepad_button::south);

inline void prepare_buffered_history(core_test_support::fixture& owners,
    std::size_t device_index = 0U) {
    auto& script = *owners.scripts[device_index];
    const auto native =
        gameinput_test_support::handle(owners.gameinput.device_memory[device_index]);
    const auto pad =
        owners.gameinput.native->current(native, gameinput_detail::native_reading_lane::gamepad);
    const auto guide = owners.gameinput.native->current(native,
        gameinput_detail::native_reading_lane::system_button);
    if (!pad.has_value() || !guide.has_value()) {
        throw std::runtime_error{"buffered fixture has no native input timestamp"};
    }
    const auto qpc = (std::max)({script.seed.baseline.observation_qpc,
        pad->observation_qpc,
        guide->observation_qpc});
    script.seed.baseline = input_update{
        .state = gameinput_test_support::test_pad(buffered_button),
        .source_timestamp_us = 100U,
        .observation_qpc = qpc,
    };
    script.qpc_frequency = 1'000'000U;
    owners.gameinput.native->set_history_source_timestamp(buffered_cut_timestamp);
}

[[nodiscard]] inline HRESULT set_buffer_size(IDirectInputDevice8W& device, DWORD requested) {
    DIPROPDWORD property{};
    property.diph.dwSize = data_size<DIPROPDWORD>();
    property.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    property.diph.dwObj = 0U;
    property.diph.dwHow = DIPH_DEVICE;
    property.dwData = requested;
    return inputnext_dinput_set_property_w(&device, &DIPROP_BUFFERSIZE, &property.diph);
}

struct buffered_pad_transition final {
    std::uint64_t timestamp{};
    button_mask buttons{};
};

inline void enqueue_pad_reading(
    const std::shared_ptr<gameinput_test_support::native_history_script>& script,
    buffered_pad_transition transition) {
    const auto state = gameinput_test_support::test_pad(transition.buttons);
    script->enqueue(gameinput_detail::native_history_step{
        .kind = gameinput_detail::native_history_step_kind::reading,
        .reading =
            input_update{
                .state = state,
                .source_timestamp_us = transition.timestamp,
                .observation_qpc = script->seed.baseline.observation_qpc + transition.timestamp -
                                   buffered_cut_timestamp,
            },
    });
}

inline void enqueue_checkpoint(
    const std::shared_ptr<gameinput_test_support::native_history_script>& script,
    std::uint64_t timestamp) {
    script->enqueue(gameinput_detail::native_history_step{
        .kind = gameinput_detail::native_history_step_kind::caught_up,
        .checkpoint_qpc =
            script->seed.baseline.observation_qpc + timestamp - buffered_cut_timestamp,
    });
}

[[nodiscard]] inline HRESULT peek_buffer_count(IDirectInputDevice8W& device, DWORD& count) {
    count = INFINITE;
    return device.GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, DIGDD_PEEK);
}

[[nodiscard]] inline bool wait_for_buffer_count(IDirectInputDevice8W& device,
    DWORD expected,
    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        DWORD count{};
        const auto result = peek_buffer_count(device, count);
        if ((result == DI_OK || result == DI_BUFFEROVERFLOW) && count >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

[[nodiscard]] inline bool wait_for_overflow(IDirectInputDevice8W& device,
    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        DWORD count{};
        if (device.GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, 0U) ==
            DI_BUFFEROVERFLOW) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

} // namespace inputnext::dinput_test_support
