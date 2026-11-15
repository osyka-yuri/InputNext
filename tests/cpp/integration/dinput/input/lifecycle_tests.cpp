#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/input_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dinput.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

TEST_CASE("dinput.concurrent_read_unacquire_reacquire_keeps_failed_outputs_unchanged",
    "[integration][dinput][input][concurrency]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x81U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(device->Acquire() == DI_OK);

    std::barrier start{2};
    std::atomic<std::uint32_t> invalid_results{};
    std::atomic<std::uint32_t> failed_writes{};
    std::jthread reader{[&] {
        start.arrive_and_wait();
        for (std::size_t iteration = 0U; iteration < 400U; ++iteration) {
            auto state = sentinel_state(0xA7U);
            const auto sentinel = state;
            const auto result = device->GetDeviceState(data_size<compact_state>(), &state);
            if (result != DI_OK && result != DIERR_NOTACQUIRED && result != DIERR_INPUTLOST) {
                invalid_results.fetch_add(1U, std::memory_order_relaxed);
            }
            if (result != DI_OK && state != sentinel) {
                failed_writes.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }};
    std::jthread toggler{[&] {
        start.arrive_and_wait();
        for (std::size_t iteration = 0U; iteration < 200U; ++iteration) {
            if (device->Unacquire() != DI_OK) {
                invalid_results.fetch_add(1U, std::memory_order_relaxed);
            }
            const auto acquired = device->Acquire();
            if (acquired != DI_OK && acquired != S_FALSE) {
                invalid_results.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    }};
    reader.join();
    toggler.join();

    CHECK(invalid_results.load(std::memory_order_relaxed) == 0U);
    CHECK(failed_writes.load(std::memory_order_relaxed) == 0U);
    CHECK(device->Acquire() == S_FALSE);
}

TEST_CASE("dinput.session_close_drains_admission_before_live_com_objects_outlast_owners",
    "[integration][dinput][input][lifecycle]") {
    auto owners = std::make_unique<core_test_support::fixture>();
    const auto target = owners->connect(0U);
    parent_unknown parent;
    auto access = make_access(*owners, *owners->focus_observer);
    auto device = create_device(target, access, parent, 0x82U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(device->Acquire() == DI_OK);

    std::promise<void> drain_started;
    const auto started_future = drain_started.get_future();
    std::promise<void> drained;
    const auto drained_future = drained.get_future();
    std::jthread drain_thread;
    auto admitted = access->enter();
    REQUIRE(static_cast<bool>(admitted));
    access->close_admission();
    CHECK_FALSE(access->open());
    CHECK_FALSE(static_cast<bool>(access->enter()));

    drain_thread = std::jthread{[&] {
        drain_started.set_value();
        access->drain();
        drained.set_value();
    }};
    CHECK(started_future.wait_for(2s) == std::future_status::ready);
    CHECK(drained_future.wait_for(20ms) == std::future_status::timeout);
    admitted = dinput::input_access::call{};
    CHECK(drained_future.wait_for(2s) == std::future_status::ready);
    drain_thread.join();

    DIDEVICEINSTANCEW info{};
    info.dwSize = data_size<DIDEVICEINSTANCEW>();
    CHECK(device->GetDeviceInfo(&info) == DIERR_INPUTLOST);
    owners.reset();

    auto state = sentinel_state(0x6CU);
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);
    CHECK(device->Unacquire() == DI_OK);
    CHECK(device.release()->Release() == 0U);
    CHECK(parent.references() == 1U);
    access.reset();
}
