#include "dinput/catalogue/discovery.hpp"
#include "dinput/control_wait.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

using namespace inputnext::dinput;
using std::chrono::steady_clock;

class unique_handle final {
public:
    explicit unique_handle(HANDLE value = nullptr) noexcept : value_(value) {}

    ~unique_handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&&) = delete;
    unique_handle& operator=(unique_handle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{};
};

constexpr UINT sent_probe_message = WM_APP + 0x71U;
constexpr UINT posted_probe_message = WM_APP + 0x72U;

struct window_state final {
    HANDLE nested_event{};
    std::uint32_t sent_count{};
    std::uint32_t posted_count{};
    HRESULT nested_status{E_UNEXPECTED};
};

[[nodiscard]] LRESULT CALLBACK test_window_procedure(HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    auto* state = std::bit_cast<window_state*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = std::bit_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<window_state*>(create->lpCreateParams);
        static_cast<void>(SetWindowLongPtrW(window, GWLP_USERDATA, std::bit_cast<LONG_PTR>(state)));
    }

    if (state != nullptr && message == sent_probe_message) {
        ++state->sent_count;
        const std::array events{state->nested_event};
        std::size_t signalled{};
        state->nested_status = wait_for_native_control(events,
            signalled,
            steady_clock::now() + std::chrono::seconds{1});
        return 0;
    }
    if (state != nullptr && message == posted_probe_message) {
        ++state->posted_count;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class hidden_window final {
public:
    explicit hidden_window(window_state& state) {
        static constexpr auto* class_name = L"InputNext.DirectInput.DiscoveryTests";
        WNDCLASSEXW window_class{};
        window_class.cbSize = static_cast<UINT>(sizeof(window_class));
        window_class.lpfnWndProc = &test_window_procedure;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name;
        if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error{"failed to register DirectInput discovery test window"};
        }

        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class_name,
            L"InputNext DirectInput discovery test",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            window_class.hInstance,
            &state);
        if (window_ == nullptr) {
            throw std::runtime_error{"failed to create DirectInput discovery test window"};
        }
    }

    ~hidden_window() {
        if (window_ != nullptr) {
            static_cast<void>(DestroyWindow(window_));
        }
    }

    hidden_window(const hidden_window&) = delete;
    hidden_window& operator=(const hidden_window&) = delete;
    hidden_window(hidden_window&&) = delete;
    hidden_window& operator=(hidden_window&&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

private:
    HWND window_{};
};

class message_flood final {
public:
    explicit message_flood(const HWND window)
        : started_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          finished_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!started_ || !finished_ || !stop_) {
            throw std::runtime_error{"failed to create sent-message test barriers"};
        }
        const HANDLE started = started_.get();
        const HANDLE finished = finished_.get();
        const HANDLE stop = stop_.get();
        sender_ = std::thread{[window, started, finished, stop] {
            static_cast<void>(SetEvent(started));
            while (WaitForSingleObject(stop, 0U) == WAIT_TIMEOUT) {
                static_cast<void>(SendMessageW(window, sent_probe_message, 0U, 0));
                static_cast<void>(WaitForSingleObject(stop, 1U));
            }
            static_cast<void>(SetEvent(finished));
        }};
    }

    ~message_flood() { finish(); }

    message_flood(const message_flood&) = delete;
    message_flood& operator=(const message_flood&) = delete;
    message_flood(message_flood&&) = delete;
    message_flood& operator=(message_flood&&) = delete;

    [[nodiscard]] bool wait_until_started() const noexcept {
        return WaitForSingleObject(started_.get(), 2'000U) == WAIT_OBJECT_0;
    }

    void finish() noexcept {
        if (!sender_.joinable()) {
            return;
        }
        static_cast<void>(SetEvent(stop_.get()));
        while (WaitForSingleObject(finished_.get(), 0U) != WAIT_OBJECT_0) {
            MSG message{};
            static_cast<void>(
                PeekMessageW(&message, nullptr, 0U, 0U, PM_NOREMOVE | PM_QS_SENDMESSAGE));
            static_cast<void>(WaitForSingleObject(finished_.get(), 1U));
        }
        sender_.join();
    }

private:
    unique_handle started_;
    unique_handle finished_;
    unique_handle stop_;
    std::thread sender_;
};

class wait_watchdog final {
public:
    explicit wait_watchdog(const HANDLE abort_event)
        : stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!stop_) {
            throw std::runtime_error{"failed to create timed wait watchdog"};
        }
        const HANDLE stop = stop_.get();
        thread_ = std::thread{[stop, abort_event] {
            if (WaitForSingleObject(stop, 2'000U) == WAIT_TIMEOUT) {
                static_cast<void>(SetEvent(abort_event));
            }
        }};
    }

    ~wait_watchdog() { stop(); }

    wait_watchdog(const wait_watchdog&) = delete;
    wait_watchdog& operator=(const wait_watchdog&) = delete;
    wait_watchdog(wait_watchdog&&) = delete;
    wait_watchdog& operator=(wait_watchdog&&) = delete;

    void stop() noexcept {
        if (thread_.joinable()) {
            static_cast<void>(SetEvent(stop_.get()));
            thread_.join();
        }
    }

private:
    unique_handle stop_;
    std::thread thread_;
};

} // namespace

TEST_CASE("dinput.discovery_waiter_prepares_before_probe_and_fans_out_notifications",
    "[component][dinput][discovery][waiter]") {
    catalogue_discovery discovery{std::chrono::seconds{30}};
    auto first = discovery.begin();
    auto second = discovery.begin();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->event() != second->event());
    CHECK(first->deadline() == second->deadline());

    REQUIRE(first->prepare());
    REQUIRE(second->prepare());
    discovery.notify();
    CHECK(WaitForSingleObject(first->event(), 0U) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(second->event(), 0U) == WAIT_OBJECT_0);

    REQUIRE(first->prepare());
    CHECK(WaitForSingleObject(first->event(), 0U) == WAIT_TIMEOUT);
    discovery.notify();
    CHECK(WaitForSingleObject(first->event(), 0U) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(second->event(), 0U) == WAIT_OBJECT_0);
}

TEST_CASE("dinput.discovery_waiters_share_one_budget_and_expire_without_renewal",
    "[component][dinput][discovery][deadline]") {
    catalogue_discovery discovery{std::chrono::seconds{10}};
    auto first = discovery.begin();
    REQUIRE(first != nullptr);
    const auto shared_deadline = first->deadline();

    auto second = discovery.begin();
    REQUIRE(second != nullptr);
    CHECK(second->deadline() == shared_deadline);
    CHECK(first->deadline() == shared_deadline);

    discovery.notify();
    REQUIRE(first->prepare());
    REQUIRE(second->prepare());
    CHECK(first->deadline() == shared_deadline);
    CHECK(second->deadline() == shared_deadline);

    catalogue_discovery immediate{std::chrono::milliseconds{0}};
    auto expired = immediate.begin();
    REQUIRE(expired != nullptr);
    CHECK(expired->deadline() <= steady_clock::now());
    CHECK_FALSE(expired->prepare());
}

TEST_CASE("dinput.discovery_starts_a_fresh_budget_after_last_waiter_detaches",
    "[component][dinput][discovery][deadline]") {
    catalogue_discovery discovery{std::chrono::milliseconds{30}};
    auto first_wave = discovery.begin();
    REQUIRE(first_wave != nullptr);
    const auto first_deadline = first_wave->deadline();
    const std::array first_event{first_wave->event()};
    std::size_t signalled{};

    CHECK(wait_for_native_control(first_event, signalled, first_deadline) ==
          HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(steady_clock::now() >= first_deadline);
    CHECK_FALSE(first_wave->prepare());
    first_wave.reset();

    auto second_wave = discovery.begin();
    REQUIRE(second_wave != nullptr);
    CHECK(second_wave->deadline() > first_deadline);
    CHECK(second_wave->prepare());
    CHECK(WaitForSingleObject(second_wave->event(), 0U) == WAIT_TIMEOUT);
}

TEST_CASE("dinput.discovery_close_is_sticky_and_waiter_outlives_coordinator",
    "[component][dinput][discovery][lifetime]") {
    auto discovery = std::make_unique<catalogue_discovery>(std::chrono::seconds{1});
    auto retained = discovery->begin();
    REQUIRE(retained != nullptr);
    const HANDLE retained_event = retained->event();

    discovery->close();
    CHECK(WaitForSingleObject(retained_event, 0U) == WAIT_OBJECT_0);
    CHECK_FALSE(retained->prepare());
    CHECK(discovery->begin() == nullptr);

    discovery->notify();
    CHECK(WaitForSingleObject(retained_event, 0U) == WAIT_OBJECT_0);
    discovery.reset();

    CHECK(WaitForSingleObject(retained->event(), 0U) == WAIT_OBJECT_0);
    CHECK_FALSE(retained->prepare());
}

TEST_CASE("dinput.control_wait_uses_absolute_deadline_and_preserves_posted_messages",
    "[component][dinput][control-wait][deadline][reentrancy]") {
    const unique_handle nested_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const unique_handle ready_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const unique_handle abort_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    REQUIRE(static_cast<bool>(nested_event));
    REQUIRE(static_cast<bool>(ready_event));
    REQUIRE(static_cast<bool>(abort_event));

    const std::array expired_events{ready_event.get()};
    std::size_t expired_index{1U};
    CHECK(wait_for_native_control(expired_events, expired_index, steady_clock::now()) ==
          HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(expired_index == 0U);

    window_state state{.nested_event = nested_event.get()};
    const hidden_window window{state};
    REQUIRE(PostMessageW(window.get(), posted_probe_message, 0U, 0) != FALSE);

    message_flood sender{window.get()};
    REQUIRE(sender.wait_until_started());
    wait_watchdog watchdog{abort_event.get()};
    const std::array events{ready_event.get(), abort_event.get()};
    const auto start = steady_clock::now();
    std::size_t signalled{};
    const auto status =
        wait_for_native_control(events, signalled, start + std::chrono::milliseconds{250});
    const auto elapsed = steady_clock::now() - start;
    watchdog.stop();
    sender.finish();

    CHECK(status == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(elapsed >= std::chrono::milliseconds{150});
    CHECK(elapsed < std::chrono::seconds{2});
    CHECK(state.sent_count >= 2U);
    CHECK(state.nested_status == E_PENDING);
    CHECK(state.posted_count == 0U);

    MSG queued{};
    CHECK(PeekMessageW(&queued,
              window.get(),
              posted_probe_message,
              posted_probe_message,
              PM_NOREMOVE) != FALSE);
    CHECK(queued.message == posted_probe_message);
    CHECK(PeekMessageW(&queued,
              window.get(),
              posted_probe_message,
              posted_probe_message,
              PM_REMOVE) != FALSE);
    CHECK(state.posted_count == 0U);
}
