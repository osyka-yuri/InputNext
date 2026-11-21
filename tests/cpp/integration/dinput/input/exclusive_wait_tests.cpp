#include "dinput/buffered/subscription.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "history/reader_observation.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::dinput;
using namespace inputnext::history_detail;

constexpr UINT sent_probe_message = WM_APP + 0x36U;
constexpr UINT posted_probe_message = WM_APP + 0x37U;

class unique_handle final {
public:
    unique_handle() noexcept = default;

    explicit unique_handle(HANDLE value) noexcept : value_(value) {}

    ~unique_handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
    }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

struct dispatch_context final {
    std::function<void()> on_sent;
    std::function<void()> on_posted;
    HWND window{};
    HRESULT nested_status{E_UNEXPECTED};
    std::uint32_t sent_count{};
    std::uint32_t posted_count{};
    bool reentrant{};
    bool nested_wait_did_not_advance{};
    bool posted{};
    bool callback_failed{};
};

[[nodiscard]] LRESULT CALLBACK test_window_procedure(HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) noexcept {
    auto* context = std::bit_cast<dispatch_context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = std::bit_cast<const CREATESTRUCTW*>(lparam);
        context = static_cast<dispatch_context*>(creation->lpCreateParams);
        static_cast<void>(
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context)));
    }

    if (context != nullptr && message == sent_probe_message) {
        ++context->sent_count;
        try {
            if (context->on_sent) {
                context->on_sent();
            }
        } catch (...) {
            context->callback_failed = true;
        }
        return 0;
    }
    if (context != nullptr && message == posted_probe_message) {
        ++context->posted_count;
        try {
            if (context->on_posted) {
                context->on_posted();
            }
        } catch (...) {
            context->callback_failed = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

class hidden_window final {
public:
    explicit hidden_window(dispatch_context& context) {
        static constexpr auto* class_name = L"InputNext.DirectInput.ExclusiveWaitTests";
        WNDCLASSEXW window_class{};
        window_class.cbSize = static_cast<UINT>(sizeof(window_class));
        window_class.lpfnWndProc = &test_window_procedure;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name;
        if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error{"failed to register exclusive wait test window class"};
        }

        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class_name,
            L"InputNext exclusive wait test window",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            window_class.hInstance,
            &context);
        if (window_ == nullptr) {
            throw std::runtime_error{"failed to create exclusive wait test window"};
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

// A real cross-thread SendMessage blocks its sender until the waiting thread
// dispatches the window procedure. finish() pumps sent messages before joining,
// so early returns and failed assertions cannot strand the sender.
class sent_message_sender final {
public:
    explicit sent_message_sender(HWND window)
        : window_{window},
          started_{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          finished_{CreateEventW(nullptr, TRUE, FALSE, nullptr)} {
        if (!started_ || !finished_) {
            throw std::runtime_error{"failed to create sent-message test barriers"};
        }
        const HANDLE started = started_.get();
        const HANDLE finished = finished_.get();
        sender_ = std::thread{[window, started, finished] {
            static_cast<void>(SetEvent(started));
            static_cast<void>(SendMessageW(window, sent_probe_message, 0U, 0));
            static_cast<void>(SetEvent(finished));
        }};
    }

    ~sent_message_sender() { finish(); }

    sent_message_sender(const sent_message_sender&) = delete;
    sent_message_sender& operator=(const sent_message_sender&) = delete;
    sent_message_sender(sent_message_sender&&) = delete;
    sent_message_sender& operator=(sent_message_sender&&) = delete;

    [[nodiscard]] bool wait_until_sending() const noexcept {
        return WaitForSingleObject(started_.get(), 2'000U) == WAIT_OBJECT_0;
    }

    void finish() noexcept {
        if (!sender_.joinable()) {
            return;
        }
        while (WaitForSingleObject(finished_.get(), 0U) != WAIT_OBJECT_0) {
            MSG message{};
            static_cast<void>(PeekMessageW(&message, window_, WM_NULL, WM_NULL, PM_NOREMOVE));
            static_cast<void>(WaitForSingleObject(finished_.get(), 1U));
        }
        sender_.join();
    }

private:
    HWND window_{};
    unique_handle started_;
    unique_handle finished_;
    std::thread sender_;
};

class cancellation_watchdog final {
public:
    explicit cancellation_watchdog(HANDLE cancellation)
        : stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
          deadline_{CreateWaitableTimerW(nullptr, TRUE, nullptr)} {
        if (!stop_ || !deadline_) {
            throw std::runtime_error{"failed to create exclusive wait watchdog"};
        }
        LARGE_INTEGER due{};
        due.QuadPart = -10'000'000LL * 10LL;
        if (SetWaitableTimer(deadline_.get(), &due, 0, nullptr, nullptr, FALSE) == FALSE) {
            throw std::runtime_error{"failed to arm exclusive wait watchdog"};
        }

        const HANDLE stop = stop_.get();
        const HANDLE timer = deadline_.get();
        thread_ = std::thread{[this, cancellation, stop, timer] {
            const std::array events{stop, timer};
            const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(events.size()),
                events.data(),
                FALSE,
                INFINITE);
            if (result == WAIT_OBJECT_0 + 1U) {
                fired_.store(true, std::memory_order_release);
                static_cast<void>(SetEvent(cancellation));
            }
        }};
    }

    ~cancellation_watchdog() { stop(); }

    cancellation_watchdog(const cancellation_watchdog&) = delete;
    cancellation_watchdog& operator=(const cancellation_watchdog&) = delete;
    cancellation_watchdog(cancellation_watchdog&&) = delete;
    cancellation_watchdog& operator=(cancellation_watchdog&&) = delete;

    [[nodiscard]] bool fired() const noexcept { return fired_.load(std::memory_order_acquire); }

    void stop() noexcept {
        if (thread_.joinable()) {
            static_cast<void>(SetEvent(stop_.get()));
            thread_.join();
        }
    }

private:
    unique_handle stop_;
    unique_handle deadline_;
    std::atomic<bool> fired_;
    std::thread thread_;
};

[[nodiscard]] connection_target test_target() noexcept {
    connection_target result{.device = device_key{1U}, .connection = connection_id{1U}};
    result.runtime.bytes.front() = std::byte{1U};
    return result;
}

[[nodiscard]] buffer_configuration test_configuration() {
    DIOBJECTDATAFORMAT object{.pguid = &GUID_Button,
        .dwOfs = 0U,
        .dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0U),
        .dwFlags = 0U};
    const DIDATAFORMAT source{.dwSize = static_cast<DWORD>(sizeof(DIDATAFORMAT)),
        .dwObjSize = static_cast<DWORD>(sizeof(DIOBJECTDATAFORMAT)),
        .dwFlags = DIDF_ABSAXIS,
        .dwDataSize = static_cast<DWORD>(sizeof(DWORD)),
        .dwNumObjs = 1U,
        .rgodf = &object};
    buffer_configuration result{.target = test_target(), .requested_size = 8U};
    if (compile_format(source, result.format, result.model) != DI_OK) {
        throw std::runtime_error{"failed to compile exclusive wait test format"};
    }
    return result;
}

[[nodiscard]] reader_boundary test_boundary() noexcept {
    return reader_boundary{.domain = history_domain{.target = test_target(), .stream = 10U},
        .sequence = 1U,
        .checkpoint_qpc = 10'000U,
        .clock = source_clock_sample{.timestamp_us = 1'000U, .system_time_ms = 20'000U}};
}

[[nodiscard]] history_sample test_sample() noexcept {
    return history_sample{.state = gamepad_state{},
        .source_timestamp_us = 100U,
        .effective_qpc = 1'000U,
        .guide_known = true};
}

struct buffered_wait_result final {
    HRESULT status{E_UNEXPECTED};
    HRESULT nested_status{E_UNEXPECTED};
    bool reentrant{};
    bool sender_started{};
    bool timed_out{};
    bool callback_failed{};
    std::uint32_t sent_count{};
};

template <typename Operation>
[[nodiscard]] buffered_wait_result run_buffered_wait(Operation operation) {
    buffered_subscription subscriber{test_configuration()};
    const unique_handle cancellation{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!cancellation) {
        throw std::runtime_error{"failed to create buffered wait cancellation event"};
    }
    cancellation_watchdog watchdog{cancellation.get()};

    dispatch_context messages{};
    HRESULT nested_status = E_UNEXPECTED;
    bool reentrant{};
    messages.on_sent = [&] {
        reentrant = native_control_reentrant();
        nested_status = subscriber.wait_ready(cancellation.get());
        operation(subscriber, cancellation.get());
    };
    const hidden_window window{messages};
    sent_message_sender sender{window.get()};
    const bool sender_started = sender.wait_until_sending();
    const HRESULT status = subscriber.wait_ready(cancellation.get());
    sender.finish();
    watchdog.stop();

    return buffered_wait_result{.status = status,
        .nested_status = nested_status,
        .reentrant = reentrant,
        .sender_started = sender_started,
        .timed_out = watchdog.fired(),
        .callback_failed = messages.callback_failed,
        .sent_count = messages.sent_count};
}

void check_wait_result(const buffered_wait_result& result, HRESULT expected) {
    CHECK(result.status == expected);
    CHECK(result.nested_status == E_PENDING);
    CHECK(result.reentrant);
    CHECK(result.sender_started);
    CHECK(!result.timed_out);
    CHECK(!result.callback_failed);
    CHECK(result.sent_count == 1U);
}

} // namespace

TEST_CASE("dinput.exclusive_control_wait_dispatches_sent_messages_only",
    "[integration][dinput][exclusive][wait][reentrancy]") {
    unique_handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    unique_handle nested{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const unique_handle deadline{CreateWaitableTimerW(nullptr, TRUE, nullptr)};
    REQUIRE(static_cast<bool>(ready));
    REQUIRE(static_cast<bool>(nested));
    REQUIRE(static_cast<bool>(deadline));

    LARGE_INTEGER due{};
    due.QuadPart = -10'000'000LL * 10LL;
    REQUIRE(SetWaitableTimer(deadline.get(), &due, 0, nullptr, nullptr, FALSE) != FALSE);

    dispatch_context messages{};
    messages.on_sent = [&] {
        messages.reentrant = native_control_reentrant();
        const std::array nested_events{nested.get()};
        std::size_t nested_index{};
        messages.nested_status = wait_for_native_control(nested_events, nested_index);
        messages.nested_wait_did_not_advance =
            WaitForSingleObject(nested.get(), 0U) == WAIT_TIMEOUT;
        messages.posted = PostMessageW(messages.window, posted_probe_message, 0U, 0) != FALSE;
        static_cast<void>(SetEvent(ready.get()));
    };
    const hidden_window window{messages};
    messages.window = window.get();
    sent_message_sender sender{window.get()};
    const bool sender_started = sender.wait_until_sending();
    const std::array events{ready.get(), deadline.get()};
    std::size_t signalled{};
    const HRESULT status = wait_for_native_control(events, signalled);
    sender.finish();

    CHECK(status == DI_OK);
    CHECK(signalled == 0U);
    CHECK(sender_started);
    CHECK(messages.sent_count == 1U);
    CHECK(messages.reentrant);
    CHECK(messages.nested_status == E_PENDING);
    CHECK(messages.nested_wait_did_not_advance);
    CHECK(messages.posted);
    CHECK(messages.posted_count == 0U);
    CHECK(!messages.callback_failed);

    MSG queued{};
    CHECK(PeekMessageW(&queued,
              window.get(),
              posted_probe_message,
              posted_probe_message,
              PM_NOREMOVE) != FALSE);
    CHECK(queued.message == posted_probe_message);
    CHECK(messages.posted_count == 0U);
    CHECK(PeekMessageW(&queued,
              window.get(),
              posted_probe_message,
              posted_probe_message,
              PM_REMOVE) != FALSE);
    CHECK(queued.message == posted_probe_message);
    CHECK(messages.posted_count == 0U);
}

TEST_CASE("dinput.exclusive_buffer_wait_returns_input_lost_when_cancelled_before_wait",
    "[integration][dinput][exclusive][wait][cancellation]") {
    buffered_subscription subscriber{test_configuration()};
    subscriber.prepare(test_boundary(), test_sample());
    const unique_handle cancellation{CreateEventW(nullptr, TRUE, TRUE, nullptr)};
    REQUIRE(static_cast<bool>(cancellation));

    CHECK(subscriber.wait_ready(cancellation.get()) == DIERR_INPUTLOST);
}

TEST_CASE("dinput.exclusive_buffer_wait_wakes_when_preparation_completes",
    "[integration][dinput][exclusive][wait][preparation]") {
    const auto result = run_buffered_wait([](buffered_subscription& subscriber, HANDLE) {
        subscriber.prepare(test_boundary(), test_sample());
    });
    check_wait_result(result, DI_OK);
}

TEST_CASE("dinput.exclusive_buffer_wait_wakes_on_input_loss",
    "[integration][dinput][exclusive][wait][loss]") {
    const auto result =
        run_buffered_wait([](buffered_subscription& subscriber, HANDLE) { subscriber.lose(); });
    check_wait_result(result, DIERR_INPUTLOST);
}

TEST_CASE("dinput.exclusive_buffer_wait_wakes_on_terminal_failure",
    "[integration][dinput][exclusive][wait][failure]") {
    const auto result = run_buffered_wait(
        [](buffered_subscription& subscriber, HANDLE) { subscriber.fail(E_ACCESSDENIED); });
    check_wait_result(result, E_ACCESSDENIED);
}

TEST_CASE("dinput.exclusive_buffer_wait_wakes_on_separate_lease_cancellation",
    "[integration][dinput][exclusive][wait][cancellation]") {
    const auto result =
        run_buffered_wait([]([[maybe_unused]] buffered_subscription& subscriber,
                              HANDLE cancellation) { static_cast<void>(SetEvent(cancellation)); });
    check_wait_result(result, DIERR_INPUTLOST);
}
