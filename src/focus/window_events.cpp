#include "window_events.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

namespace inputnext::focus::detail {
namespace {

using namespace std::chrono_literals;

constexpr auto pump_interval = 4ms;
constexpr auto heartbeat_budget = 250ms;

[[nodiscard]] std::int64_t clock_nanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct window_events::implementation final {
    implementation() noexcept : stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (stop_event == nullptr) {
            return;
        }
        try {
            worker = std::thread{[this] { run(); }};
        } catch (...) {
            return;
        }
    }

    ~implementation() {
        if (worker.joinable()) {
            static_cast<void>(SetEvent(stop_event));
            worker.join();
        }
        if (stop_event != nullptr) {
            static_cast<void>(CloseHandle(stop_event));
        }
    }

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    // Windows fixes this callback's parameter order.
    static void CALLBACK on_event([[maybe_unused]] HWINEVENTHOOK event_hook,
        DWORD event,
        [[maybe_unused]] HWND window,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): WinEventProc parameter order is fixed by Windows.
        [[maybe_unused]] LONG object_id,
        [[maybe_unused]] LONG child_id,
        [[maybe_unused]] DWORD event_thread,
        [[maybe_unused]] DWORD event_time) noexcept {
        if (event == EVENT_SYSTEM_FOREGROUND && callback_owner != nullptr) {
            callback_owner->advance_transition();
        }
    }

    void advance_transition() noexcept {
        auto current = transition.load(std::memory_order_relaxed);
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (transition.compare_exchange_weak(current,
                    current + 1U,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
        }
        hook_available.store(false, std::memory_order_release);
    }

    void run() noexcept {
        if (WaitForSingleObject(stop_event, 0U) == WAIT_OBJECT_0) {
            return;
        }
        MSG initial_message{};
        static_cast<void>(PeekMessageW(&initial_message, nullptr, 0U, 0U, PM_NOREMOVE));

        callback_owner = this;
        auto* const hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &on_event,
            0U,
            0U,
            WINEVENT_OUTOFCONTEXT);
        if (hook == nullptr) {
            callback_owner = nullptr;
            return;
        }

        heartbeat.store(clock_nanoseconds(), std::memory_order_release);
        hook_available.store(true, std::memory_order_release);

        bool keep_running = true;
        while (keep_running) {
            const auto result = MsgWaitForMultipleObjects(1U,
                &stop_event,
                FALSE,
                static_cast<DWORD>(pump_interval.count()),
                QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) {
                break;
            }
            if (result == WAIT_FAILED) {
                break;
            }

            MSG message{};
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
                if (message.message == WM_QUIT) {
                    keep_running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            heartbeat.store(clock_nanoseconds(), std::memory_order_release);
        }

        hook_available.store(false, std::memory_order_release);
        static_cast<void>(UnhookWinEvent(hook));
        callback_owner = nullptr;
    }

    static thread_local implementation* callback_owner;

    HANDLE stop_event{};
    std::thread worker;
    std::atomic<std::uint64_t> transition;
    std::atomic<std::int64_t> heartbeat;
    std::atomic<bool> hook_available;
};

thread_local window_events::implementation* window_events::implementation::callback_owner = nullptr;

window_events::window_events() noexcept {
    try {
        implementation_ = std::make_unique<implementation>();
    } catch (...) {
        implementation_.reset();
    }
}

window_events::~window_events() = default;

window_events::sample window_events::read() const noexcept {
    if (implementation_ == nullptr) {
        return {};
    }

    const auto transition = implementation_->transition.load(std::memory_order_acquire);
    const auto heartbeat = implementation_->heartbeat.load(std::memory_order_acquire);
    const auto now = clock_nanoseconds();
    const bool fresh =
        now >= heartbeat &&
        now - heartbeat <
            std::chrono::duration_cast<std::chrono::nanoseconds>(heartbeat_budget).count();
    return sample{
        .transition = transition,
        .available = implementation_->hook_available.load(std::memory_order_acquire) && fresh &&
                     transition != std::numeric_limits<std::uint64_t>::max(),
    };
}

} // namespace inputnext::focus::detail
