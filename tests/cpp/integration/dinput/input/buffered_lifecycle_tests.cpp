#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/focus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <thread>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

class scoped_handle final {
public:
    explicit scoped_handle(HANDLE value = nullptr) noexcept : value_(value) {}

    ~scoped_handle() {
        if (value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    scoped_handle(const scoped_handle&) = delete;
    scoped_handle& operator=(const scoped_handle&) = delete;

    scoped_handle(scoped_handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    scoped_handle& operator=(scoped_handle&& other) noexcept {
        if (this != &other) {
            close();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    void close() noexcept {
        if (value_ != nullptr) {
            CloseHandle(value_);
            value_ = nullptr;
        }
    }

private:
    HANDLE value_{};
};

[[nodiscard]] scoped_handle make_event() noexcept {
    return scoped_handle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
}

[[nodiscard]] scoped_handle duplicate_event(HANDLE event) noexcept {
    HANDLE duplicate{};
    if (DuplicateHandle(GetCurrentProcess(),
            event,
            GetCurrentProcess(),
            &duplicate,
            0U,
            FALSE,
            DUPLICATE_SAME_ACCESS) == FALSE) {
        return scoped_handle{};
    }
    return scoped_handle{duplicate};
}

} // namespace

TEST_CASE("dinput.event_only_subscription_duplicates_and_replaces_handles_safely",
    "[integration][dinput][input][buffered][notification]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x74U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());

    auto original_event = make_event();
    const auto original_waiter = duplicate_event(original_event.get());
    auto replacement_event = make_event();
    const auto replacement_waiter = duplicate_event(replacement_event.get());
    REQUIRE(original_event.get() != nullptr);
    REQUIRE(original_waiter.get() != nullptr);
    REQUIRE(replacement_event.get() != nullptr);
    REQUIRE(replacement_waiter.get() != nullptr);

    REQUIRE(device->SetEventNotification(original_event.get()) == DI_OK);
    REQUIRE(device->SetEventNotification(replacement_event.get()) == DI_OK);
    original_event.close();
    replacement_event.close();
    REQUIRE(device->Acquire() == DI_OK);

    DIDEVICEOBJECTDATA event{};
    DWORD count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), &event, &count, 0U) ==
          DIERR_NOTBUFFERED);
    CHECK(count == 0U);

    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 1U);
    CHECK(WaitForSingleObject(replacement_waiter.get(), 2'000U) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(original_waiter.get(), 0U) == WAIT_TIMEOUT);
    CHECK(device->SetEventNotification(nullptr) == DIERR_ACQUIRED);

    REQUIRE(device->Unacquire() == DI_OK);
    REQUIRE(device->SetEventNotification(nullptr) == DI_OK);
    REQUIRE(ResetEvent(replacement_waiter.get()) != FALSE);
    enqueue_pad_reading(script,
        {.timestamp = buffered_cut_timestamp + 2U, .buttons = buffered_button});
    enqueue_checkpoint(script, buffered_cut_timestamp + 2U);
    CHECK(WaitForSingleObject(replacement_waiter.get(), 100U) == WAIT_TIMEOUT);
}

TEST_CASE("dinput.event_registration_rejects_non_events_without_changing_the_selected_event",
    "[integration][dinput][input][buffered][notification][arguments]") {
    core_test_support::fixture owners;
    const auto target = owners.connect();
    prepare_buffered_history(owners);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x83U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    const auto event = make_event();
    const scoped_handle mutex{CreateMutexW(nullptr, FALSE, nullptr)};
    const scoped_handle semaphore{CreateSemaphoreW(nullptr, 0L, 1L, nullptr)};
    const scoped_handle timer{CreateWaitableTimerW(nullptr, TRUE, nullptr)};
    const scoped_handle process{OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId())};
    REQUIRE(event.get() != nullptr);
    REQUIRE(mutex.get() != nullptr);
    REQUIRE(semaphore.get() != nullptr);
    REQUIRE(timer.get() != nullptr);
    REQUIRE(process.get() != nullptr);
    REQUIRE(device->SetEventNotification(event.get()) == DI_OK);
    for (auto* const invalid : {mutex.get(), semaphore.get(), timer.get(), process.get()}) {
        const auto result = device->SetEventNotification(invalid);
        CHECK(result == E_HANDLE);
        if (SUCCEEDED(result)) {
            REQUIRE(device->SetEventNotification(event.get()) == DI_OK);
        }
    }
    REQUIRE(device->Acquire() == DI_OK);
    const auto script = owners.scripts[0U];
    enqueue_pad_reading(script, {.timestamp = buffered_cut_timestamp + 1U, .buttons = 0U});
    enqueue_checkpoint(script, buffered_cut_timestamp + 1U);
    CHECK(WaitForSingleObject(event.get(), 2'000U) == WAIT_OBJECT_0);
    CHECK(device->Unacquire() == DI_OK);

    for (const bool manual : {false, true}) {
        const scoped_handle stateful{CreateEventW(nullptr, manual ? TRUE : FALSE, TRUE, nullptr)};
        REQUIRE(stateful.get() != nullptr);
        REQUIRE(device->SetEventNotification(stateful.get()) == DI_OK);
        CHECK(WaitForSingleObject(stateful.get(), 0U) == WAIT_OBJECT_0);
        REQUIRE(ResetEvent(stateful.get()) != FALSE);
        REQUIRE(device->SetEventNotification(stateful.get()) == DI_OK);
        CHECK(WaitForSingleObject(stateful.get(), 0U) == WAIT_TIMEOUT);
    }
    CHECK(device->SetEventNotification(nullptr) == DI_OK);
}

TEST_CASE("dinput.blocked_history_does_not_block_ready_reads_other_lanes_or_unacquire",
    "[integration][dinput][input][buffered][concurrency]") {
    core_test_support::fixture owners;
    const auto blocked_target = owners.connect(0U);
    const auto ready_target = owners.connect(1U);
    prepare_buffered_history(owners, 0U);
    prepare_buffered_history(owners, 1U);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    auto blocked_device = create_device(blocked_target, access, parent, 0x75U);
    auto ready_device = create_device(ready_target, access, parent, 0x76U);
    configure_compact_device(*blocked_device.get(), window.get());
    configure_compact_device(*ready_device.get(), window.get());
    REQUIRE(set_buffer_size(*blocked_device.get(), 8U) == DI_OK);
    REQUIRE(set_buffer_size(*ready_device.get(), 8U) == DI_OK);
    REQUIRE(blocked_device->Acquire() == DI_OK);
    REQUIRE(ready_device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(2s));

    auto cached_read = std::async(std::launch::async, [&] {
        compact_state state{};
        return blocked_device->GetDeviceState(data_size<compact_state>(), &state);
    });
    auto other_lane_read = std::async(std::launch::async, [&] {
        DWORD available = INFINITE;
        return ready_device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
            nullptr,
            &available,
            DIGDD_PEEK);
    });
    const auto cached_status = cached_read.wait_for(500ms);
    const auto other_lane_status = other_lane_read.wait_for(500ms);

    auto unacquire = std::async(std::launch::async, [&] { return blocked_device->Unacquire(); });
    const auto unacquire_status = unacquire.wait_for(500ms);
    release.release();

    CHECK(cached_status == std::future_status::ready);
    CHECK(other_lane_status == std::future_status::ready);
    CHECK(unacquire_status == std::future_status::ready);
    if (cached_status == std::future_status::ready) {
        CHECK(cached_read.get() == DI_OK);
    } else {
        static_cast<void>(cached_read.get());
    }
    if (other_lane_status == std::future_status::ready) {
        CHECK(other_lane_read.get() == DI_OK);
    } else {
        static_cast<void>(other_lane_read.get());
    }
    if (unacquire_status == std::future_status::ready) {
        CHECK(unacquire.get() == DI_OK);
    } else {
        static_cast<void>(unacquire.get());
    }
    CHECK(ready_device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.unacquire_cancels_a_pending_acquire_while_native_history_is_blocked",
    "[integration][dinput][input][buffered][cancellation]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x79U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    auto acquiring = std::async(std::launch::async, [&] { return device->Acquire(); });
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(2s));

    CHECK(device->Unacquire() == DI_OK);
    const auto cancelled = acquiring.wait_for(500ms);
    release.release();
    CHECK(cancelled == std::future_status::ready);
    CHECK(acquiring.get() == DIERR_INPUTLOST);
    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_NOTACQUIRED);
}

TEST_CASE("dinput.close_admission_cancels_subscriptions_before_history_drain",
    "[integration][dinput][input][buffered][shutdown]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x7AU);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(2s));

    access->close_admission();
    CHECK_FALSE(access->open());
    CHECK_FALSE(static_cast<bool>(access->enter()));
    auto state = sentinel_state(0x61U);
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);

    std::promise<void> drained;
    const auto drained_future = drained.get_future();
    std::jthread drain_thread{[&] {
        access->drain();
        drained.set_value();
    }};
    CHECK(drained_future.wait_for(20ms) == std::future_status::timeout);
    release.release();
    CHECK(drained_future.wait_for(2s) == std::future_status::ready);
    drain_thread.join();
    CHECK(device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.foreground_sweep_signals_loss_while_native_history_is_blocked",
    "[integration][dinput][input][buffered][focus]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);
    focus_window_fixture foreground;
    const hidden_window window;
    foreground.set_window(window.id(), 2U);
    REQUIRE(foreground.wait_for_window(window.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, foreground.observer());
    auto device = create_device(target, access, parent, 0x7BU);
    configure_compact_device(*device.get(), window.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    const auto event = make_event();
    REQUIRE(event.get() != nullptr);
    REQUIRE(device->SetEventNotification(event.get()) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(2s));

    foreground.set_foreground(false, window.id(), 3U);
    REQUIRE(foreground.wait_for_evidence(focus::evidence::background));
    access->refresh_eligibility();
    CHECK(WaitForSingleObject(event.get(), 2'000U) == WAIT_OBJECT_0);

    auto state = sentinel_state(0x7CU);
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);
    foreground.set_window(window.id(), 4U);
    REQUIRE(foreground.wait_for_window(window.id(), 4U));
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);
    CHECK(device->Unacquire() == DI_OK);
    release.release();
}

TEST_CASE("dinput.buffered_old_connection_never_attaches_to_a_reconnected_device",
    "[integration][dinput][input][buffered][reconnect]") {
    core_test_support::fixture owners;
    const auto old_target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    auto old_device = create_device(old_target, access, parent, 0x7DU);
    configure_compact_device(*old_device.get(), window.get());
    REQUIRE(set_buffer_size(*old_device.get(), 8U) == DI_OK);
    REQUIRE(old_device->Acquire() == DI_OK);

    owners.gameinput.native->emit(
        gameinput_test_support::handle(owners.gameinput.device_memory[0U]),
        gameinput_detail::native_device_event::disconnected);
    for (std::size_t attempt = 0U;
        attempt < 32U && owners.gameinput.runtime->connections().size != 0U;
        ++attempt) {
        owners.gameinput.maintain();
    }
    REQUIRE(owners.gameinput.runtime->connections().size == 0U);
    const auto new_target = owners.connect(0U);
    REQUIRE(new_target != old_target);

    std::array<DIDEVICEOBJECTDATA, 2U> events{};
    auto count = static_cast<DWORD>(events.size());
    CHECK(old_device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) ==
          DIERR_INPUTLOST);
    CHECK(old_device->Acquire() == DIERR_INPUTLOST);

    auto new_device = create_device(new_target, access, parent, 0x7EU);
    configure_compact_device(*new_device.get(), window.get());
    REQUIRE(set_buffer_size(*new_device.get(), 8U) == DI_OK);
    REQUIRE(new_device->Acquire() == DI_OK);
    compact_state state{};
    CHECK(new_device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}
