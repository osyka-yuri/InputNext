#include "gameinput/native/native_gameinput.hpp"

#include "gameinput/reading_parts.hpp"

#include <Windows.h>
#include <cstdio>
#include <exception>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class unique_handle final {
public:
    explicit unique_handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~unique_handle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&&) = delete;
    unique_handle& operator=(unique_handle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{};
};

struct callback_state final {
    inputnext::gameinput_detail::native_gameinput* native{};
    HANDLE device_found{};
    HANDLE reading_entered{};
    HANDLE release_reading{};
    HANDLE unregistered{};
    std::atomic<void*> device{nullptr};
    std::atomic<bool> hold_requested{false};
    std::atomic<bool> unregister_result{false};
    std::atomic<unsigned int> readings_exited{0U};
};

void device_arrived(void* owner,
    inputnext::gameinput_detail::native_device_handle device,
    inputnext::gameinput_detail::native_device_event event) noexcept {
    if (event != inputnext::gameinput_detail::native_device_event::connected || !device.valid()) {
        return;
    }

    auto& state = *static_cast<callback_state*>(owner);
    state.native->retain(device);
    void* expected{};
    if (!state.device.compare_exchange_strong(expected,
            device.value,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        state.native->release(device);
        return;
    }
    (void)SetEvent(state.device_found);
}

void hold_reading_callback(void* owner,
    const inputnext::gameinput_detail::reading_parts& reading) noexcept {
    (void)reading;
    auto& state = *static_cast<callback_state*>(owner);
    if (!state.hold_requested.load(std::memory_order_acquire)) {
        return;
    }
    (void)SetEvent(state.reading_entered);
    (void)WaitForSingleObject(state.release_reading, INFINITE);
    state.readings_exited.fetch_add(1U, std::memory_order_release);
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(256U);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), length}}.parent_path();
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

[[noreturn]] void end_without_cleanup(unsigned int exit_code) noexcept {
    (void)TerminateProcess(GetCurrentProcess(), exit_code);
    ExitProcess(exit_code);
}

class callback_unwind_guard final {
public:
    callback_unwind_guard() = default;

    ~callback_unwind_guard() {
        // Keep callback contexts and the unregister thread alive on an unexpected exception.
        if (std::uncaught_exceptions() != 0) {
            end_without_cleanup(13U);
        }
    }

    callback_unwind_guard(const callback_unwind_guard&) = delete;
    callback_unwind_guard& operator=(const callback_unwind_guard&) = delete;
    callback_unwind_guard(callback_unwind_guard&&) = delete;
    callback_unwind_guard& operator=(callback_unwind_guard&&) = delete;
};

void inject_requested_exception(int argc,
    wchar_t** argv,
    callback_state& state,
    const inputnext::gameinput_detail::native_reading_callback_context& context,
    std::thread& worker) {
    if (argc == 2 && std::wstring_view{argv[1]} == L"--verify-unwind-guard") {
        // Report premature thread destruction without a CRT error dialog.
        std::set_terminate([] { end_without_cleanup(14U); });
        state.hold_requested.store(true, std::memory_order_release);
        worker = std::thread{[&context] { context.deliver(context.owner, {}); }};
        if (WaitForSingleObject(state.reading_entered, 2'000U) != WAIT_OBJECT_0 ||
            !worker.joinable() || state.readings_exited.load(std::memory_order_acquire) != 0U) {
            end_without_cleanup(14U);
        }
        static_cast<void>(
            std::fputs("reading_callback_entered=1 worker_joinable=1 reading_callback_exited=0\n",
                stdout));
        static_cast<void>(std::fflush(stdout));
        throw std::runtime_error{"callback unwind guard"};
    }
}

void unregister_or_exit(inputnext::gameinput_detail::native_gameinput& native,
    inputnext::gameinput_detail::native_callback_token token,
    unsigned int failure_code) noexcept {
    if (!native.unregister_callback(token)) {
        end_without_cleanup(failure_code);
    }
}

} // namespace

int wmain(const int argc, wchar_t** argv) try {
    using namespace inputnext::gameinput_detail;
    const unique_handle device_found{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const unique_handle reading_entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const unique_handle release_reading{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    unique_handle unregistered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (device_found.get() == nullptr || reading_entered.get() == nullptr ||
        release_reading.get() == nullptr || unregistered.get() == nullptr) {
        std::cout << "event_setup=failed error=" << GetLastError() << '\n';
        return 2;
    }

    auto native = make_native_gameinput();
    if (!native) {
        std::cout << "adapter=create_failed\n";
        return 3;
    }
    callback_state state{
        .native = native.get(),
        .device_found = device_found.get(),
        .reading_entered = reading_entered.get(),
        .release_reading = release_reading.get(),
        .unregistered = unregistered.get(),
    };
    native_device_callback_context device_context{.owner = &state, .deliver = device_arrived};
    native_reading_callback_context reading_context{.owner = &state,
        .deliver = hold_reading_callback};
    std::thread unregister_thread;
    const callback_unwind_guard abort_on_unwind;
    inject_requested_exception(argc, argv, state, reading_context, unregister_thread);

    const auto directory = executable_directory();
    if (directory.empty()) {
        std::cout << "executable_path=failed\n";
        return 3;
    }
    const auto initialized = native->initialize(directory / L"GameInputRedist.dll");
    if (!initialized.has_value()) {
        std::cout << "adapter=initialize_failed code=" << initialized.error().native_code << '\n';
        return 4;
    }

    const auto watch = [&] -> native_result<native_callback_token> {
        try {
            return native->register_device_watch(device_context);
        } catch (...) {
            end_without_cleanup(5U);
        }
    }();
    if (!watch.has_value()) {
        if (watch.error().partial_token.valid()) {
            unregister_or_exit(*native, watch.error().partial_token, 5U);
        }
        std::cout << "watch=registration_failed code=" << watch.error().native_code << '\n';
        return 5;
    }

    if (WaitForSingleObject(device_found.get(), 8'000U) != WAIT_OBJECT_0) {
        unregister_or_exit(*native, watch.value(), 6U);
        std::cout << "device=not_observed\n";
        return 6;
    }

    const native_device_handle device{state.device.load(std::memory_order_acquire)};
    const auto reading = [&] -> native_result<native_callback_token> {
        try {
            return native->register_reading_callback(device, reading_context);
        } catch (...) {
            end_without_cleanup(7U);
        }
    }();
    if (!reading.has_value()) {
        if (reading.error().partial_token.valid()) {
            unregister_or_exit(*native, reading.error().partial_token, 7U);
        }
        unregister_or_exit(*native, watch.value(), 7U);
        native->release(device);
        std::cout << "reading=registration_failed code=" << reading.error().native_code << '\n';
        return 7;
    }

    state.hold_requested.store(true, std::memory_order_release);
    if (WaitForSingleObject(reading_entered.get(), 8'000U) != WAIT_OBJECT_0) {
        state.hold_requested.store(false, std::memory_order_release);
        (void)SetEvent(release_reading.get());
        unregister_or_exit(*native, reading.value(), 8U);
        unregister_or_exit(*native, watch.value(), 8U);
        native->release(device);
        std::cout << "reading_callback=not_observed\n";
        return 8;
    }

    try {
        unregister_thread = std::thread{[&, token = reading.value()] {
            state.unregister_result.store(native->unregister_callback(token),
                std::memory_order_release);
            (void)SetEvent(unregistered.get());
        }};
    } catch (...) {
        end_without_cleanup(9U);
    }
    const bool remained_pending = WaitForSingleObject(unregistered.get(), 100U) == WAIT_TIMEOUT;
    (void)SetEvent(release_reading.get());
    if (WaitForSingleObject(unregistered.get(), 8'000U) != WAIT_OBJECT_0) {
        std::cout << "reading_unregister=did_not_complete\n" << std::flush;
        end_without_cleanup(9U);
    }
    unregister_thread.join();

    const bool reading_unregistered = state.unregister_result.load(std::memory_order_acquire);
    const unsigned int exited = state.readings_exited.load(std::memory_order_acquire);
    if (!reading_unregistered || exited == 0U) {
        std::cout << "reading_unregister=failed success=" << reading_unregistered
                  << " exited=" << exited << '\n'
                  << std::flush;
        end_without_cleanup(10U);
    }
    unregister_or_exit(*native, watch.value(), 11U);

    native->release(device);
    std::cout << "device_callback_entered=1 reading_callback_entered=1 unregister_pending="
              << remained_pending
              << " reading_unregister_succeeded=1 reading_callback_exited=1 "
                 "device_watch_unregistered=1\n";
    return remained_pending ? 0 : 12;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "native_worker_error=%s\n", error.what()));
    return 13;
} catch (...) {
    static_cast<void>(std::fputs("native_worker_error=unknown exception\n", stderr));
    return 13;
}
