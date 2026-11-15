#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <dinput.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

using namespace inputnext;
using namespace inputnext::dinput_test_support;

TEST_CASE("dinput.buffer_size_one_accepts_acquisition_and_reports_lost_events",
    "[integration][dinput][input][buffered][capacity]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x7FU);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 1U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 1U);
    REQUIRE(wait_for_overflow(*device.get()));

    DWORD count = INFINITE;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, DIGDD_PEEK) ==
          DI_BUFFEROVERFLOW);
    CHECK(count == 0U);

    std::array<DIDEVICEOBJECTDATA, 1U> event{};
    count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), event.data(), &count, 0U) ==
          DI_BUFFEROVERFLOW);
    CHECK(count == 0U);
    count = 0U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), nullptr, &count, 0U) == DI_OK);
}

TEST_CASE("dinput.buffered_subscriber_limit_recovers_after_release",
    "[integration][dinput][input][buffered][capacity]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    constexpr std::size_t subscriber_limit = 64U;
    constexpr std::size_t attempted_subscribers = subscriber_limit + 1U;
    constexpr DWORD requested_capacity = 4'096U;
    static_assert(
        subscriber_limit * 4'095U * sizeof(DIDEVICEOBJECTDATA) < std::size_t{8U} * 1024U * 1024U);
    std::array<com_owner<IDirectInputDevice8W>, attempted_subscribers> devices{};

    for (std::size_t index = 0U; index < subscriber_limit; ++index) {
        devices[index] =
            create_device(target, access, parent, static_cast<std::uint8_t>(index + 1U));
        configure_compact_device(*devices[index].get(), window.get());
        REQUIRE(set_buffer_size(*devices[index].get(), requested_capacity) == DI_OK);
        const auto acquired = devices[index]->Acquire();
        if (acquired != DI_OK) {
            INFO("subscriber index: " << index);
        }
        REQUIRE(acquired == DI_OK);
    }

    devices[subscriber_limit] = create_device(target, access, parent, 0xFEU);
    configure_compact_device(*devices[subscriber_limit].get(), window.get());
    REQUIRE(set_buffer_size(*devices[subscriber_limit].get(), requested_capacity) == DI_OK);
    CHECK(devices[subscriber_limit]->Acquire() == DIERR_OUTOFMEMORY);

    REQUIRE(devices[0U]->Unacquire() == DI_OK);
    devices[0U].reset();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    HRESULT retry{};
    do {
        retry = devices[subscriber_limit]->Acquire();
        if (retry == DIERR_OUTOFMEMORY) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    } while (retry == DIERR_OUTOFMEMORY && std::chrono::steady_clock::now() < deadline);
    CHECK(retry == DI_OK);

    for (auto& device : devices) {
        if (device) {
            CHECK(device->Unacquire() == DI_OK);
        }
    }
}

TEST_CASE("dinput.buffered_lane_limit_includes_a_blocked_retired_connection_until_drain",
    "[integration][dinput][input][buffered][capacity][reconnect]") {
    core_test_support::fixture owners;
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    std::array<com_owner<IDirectInputDevice8W>, gameinput_live_connection_capacity> devices{};
    std::array<connection_target, gameinput_live_connection_capacity> targets{};
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        const auto target = owners.connect(index);
        targets[index] = target;
        prepare_buffered_history(owners, index);
        devices[index] =
            create_device(target, access, parent, static_cast<std::uint8_t>(index + 1U));
        configure_compact_device(*devices[index].get(), window.get());
        REQUIRE(set_buffer_size(*devices[index].get(), 8U) == DI_OK);
        REQUIRE(devices[index]->Acquire() == DI_OK);
    }

    const auto script = owners.scripts[0U];
    script->block_next();
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(std::chrono::seconds{2}));
    owners.gameinput.native->emit(
        gameinput_test_support::handle(owners.gameinput.device_memory[0U]),
        gameinput_detail::native_device_event::disconnected);
    owners.gameinput.maintain();
    REQUIRE(owners.gameinput.runtime->connections().size == devices.size() - 1U);

    static_cast<void>(owners.connect(0U));
    const auto inventory = owners.gameinput.runtime->connections();
    connection_target replacement_target{};
    for (const auto& connection : std::span{inventory.entries}.first(inventory.size)) {
        if (std::ranges::find(targets, connection.target) == targets.end()) {
            replacement_target = connection.target;
        }
    }
    REQUIRE(replacement_target.valid());
    auto replacement = create_device(replacement_target, access, parent, 0xFEU);
    configure_compact_device(*replacement.get(), window.get());
    REQUIRE(set_buffer_size(*replacement.get(), 8U) == DI_OK);
    CHECK(replacement->Acquire() == DIERR_OUTOFMEMORY);
    REQUIRE(devices[0U]->Unacquire() == DI_OK);
    devices[0U].reset();
    release.release();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    HRESULT acquired{};
    do {
        acquired = replacement->Acquire();
        if (acquired == DIERR_OUTOFMEMORY) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    } while (acquired == DIERR_OUTOFMEMORY && std::chrono::steady_clock::now() < deadline);
    CHECK(acquired == DI_OK);
}
