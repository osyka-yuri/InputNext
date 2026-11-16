#pragma once

#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/com/device.hpp"
#include "dinput/device_info.hpp"
#include "dinput/input_access.hpp"
#include "dinput/profile.hpp"
#include "dinput/system_input.hpp"
#include "focus/observer_test_access.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "support/focus/manual_clock.hpp"

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::dinput_test_support {

using namespace std::chrono_literals;

template <typename Value>
[[nodiscard]] constexpr DWORD data_size() noexcept {
    return static_cast<DWORD>(sizeof(Value));
}

class parent_unknown final : public IUnknown {
public:
    explicit parent_unknown(std::atomic<std::uint32_t>* destroyed = nullptr) noexcept
        : destroyed_(destroyed) {}

    parent_unknown(const parent_unknown&) = delete;
    parent_unknown& operator=(const parent_unknown&) = delete;
    parent_unknown(parent_unknown&&) = delete;
    parent_unknown& operator=(parent_unknown&&) = delete;

    ~parent_unknown() {
        if (destroyed_ != nullptr) {
            destroyed_->fetch_add(1U, std::memory_order_relaxed);
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (iid != IID_IUnknown) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return references_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = references_.fetch_sub(1U, std::memory_order_acq_rel) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    [[nodiscard]] ULONG references() const noexcept {
        return references_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint32_t>* destroyed_{};
    std::atomic<ULONG> references_{1U};
};

template <typename Interface>
class com_owner final {
public:
    com_owner() noexcept = default;

    explicit com_owner(Interface* value) noexcept : value_(value) {}

    ~com_owner() {
        if (value_ != nullptr) {
            value_->Release();
        }
    }

    com_owner(const com_owner&) = delete;
    com_owner& operator=(const com_owner&) = delete;

    com_owner(com_owner&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    com_owner& operator=(com_owner&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] Interface* get() const noexcept { return value_; }

    [[nodiscard]] Interface* operator->() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

    [[nodiscard]] Interface* release() noexcept { return std::exchange(value_, nullptr); }

    void reset(Interface* replacement = nullptr) noexcept {
        if (value_ != nullptr) {
            value_->Release();
        }
        value_ = replacement;
    }

private:
    Interface* value_{};
};

template <typename Interface>
[[nodiscard]] Interface& operator*(const com_owner<Interface>& value) noexcept {
    return *value.get();
}

struct compact_state final {
    std::array<LONG, 6U> axes{};
    DWORD pov{};
    std::array<BYTE, 15U> buttons{};

    friend constexpr bool operator==(const compact_state&, const compact_state&) noexcept = default;
};

static_assert(sizeof(compact_state) % sizeof(DWORD) == 0U);

[[nodiscard]] inline DIDATAFORMAT compact_format(std::array<DIOBJECTDATAFORMAT, 22U>& descriptors,
    std::array<GUID, 22U>& guids) noexcept {
    const auto profile = dinput::objects();
    for (std::size_t index = 0U; index < profile.size(); ++index) {
        const auto& object = profile[index];
        guids[index] = object.guid;
        DWORD offset{};
        switch (object.kind) {
        case dinput::object_kind::axis:
            offset = static_cast<DWORD>(
                offsetof(compact_state, axes) + (object.source_index * sizeof(LONG)));
            break;
        case dinput::object_kind::button:
            offset = static_cast<DWORD>(offsetof(compact_state, buttons) + object.source_index);
            break;
        case dinput::object_kind::pov:
            offset = static_cast<DWORD>(offsetof(compact_state, pov));
            break;
        }
        descriptors[index] = DIOBJECTDATAFORMAT{};
        descriptors[index].pguid = &guids[index];
        descriptors[index].dwOfs = offset;
        descriptors[index].dwType = object.id;
        descriptors[index].dwFlags =
            object.kind == dinput::object_kind::axis ? DIDOI_ASPECTPOSITION : 0U;
    }

    DIDATAFORMAT format{};
    format.dwSize = data_size<DIDATAFORMAT>();
    format.dwObjSize = data_size<DIOBJECTDATAFORMAT>();
    format.dwFlags = DIDF_ABSAXIS;
    format.dwDataSize = data_size<compact_state>();
    format.dwNumObjs = static_cast<DWORD>(descriptors.size());
    format.rgodf = descriptors.data();
    return format;
}

[[nodiscard]] inline compact_state sentinel_state(const BYTE value) noexcept {
    compact_state result{};
    result.axes.fill(value);
    result.pov = static_cast<DWORD>(value) * 0x01010101U;
    result.buttons.fill(value);
    return result;
}

[[nodiscard]] inline gamepad_state sample_state() noexcept {
    using inputnext::gamepad_button;
    return gamepad_state{
        .buttons = static_cast<button_mask>(gamepad_button::south) |
                   static_cast<button_mask>(gamepad_button::east) |
                   static_cast<button_mask>(gamepad_button::dpad_up) |
                   static_cast<button_mask>(gamepad_button::dpad_right),
        .left_stick = {.x = 0.25F, .y = -0.25F},
        .right_stick = {.x = -0.25F, .y = 0.25F},
        .left_trigger = 0.25F,
        .right_trigger = 0.75F,
    };
}

[[nodiscard]] inline inputnext::dinput::device_identity identity_for(const connection_target target,
    const std::uint8_t instance_byte = 0x31U) noexcept {
    dinput::device_identity identity{};
    identity.target = target;
    identity.instance.dwSize = data_size<DIDEVICEINSTANCEW>();
    identity.instance.guidInstance = GUID{.Data1 = 0x1a2b3c00U + instance_byte,
        .Data2 = 0x9d11U,
        .Data3 = 0x4c73U,
        .Data4 = {0x82U, 0x15U, 0x52U, 0x61U, 0x33U, 0x14U, 0x10U, instance_byte}};
    identity.instance.guidProduct = GUID_Joystick;
    identity.instance.dwDevType = DI8DEVTYPE_GAMEPAD;
    wcscpy_s(identity.instance.tszInstanceName, L"InputNext integration gamepad");
    wcscpy_s(identity.instance.tszProductName, L"InputNext test controller");
    identity.vendor_product = 0x054c0ce6U;
    return identity;
}

[[nodiscard]] inline com_owner<IDirectInputDevice8W> create_device(const connection_target target,
    const std::shared_ptr<dinput::input_access>& access,
    IUnknown& parent,
    const std::uint8_t instance_byte = 0x31U) {
    void* created{};
    const auto status =
        dinput::make_device(identity_for(target, instance_byte), access, parent, true, &created);
    if (FAILED(status)) {
        throw std::runtime_error{"DirectInput device creation failed"};
    }
    return com_owner<IDirectInputDevice8W>{static_cast<IDirectInputDevice8W*>(created)};
}

inline void configure_compact_device(IDirectInputDevice8W& device,
    const HWND window,
    const DWORD cooperation = DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) {
    std::array<DIOBJECTDATAFORMAT, 22U> descriptors{};
    std::array<GUID, 22U> guids{};
    const auto format = compact_format(descriptors, guids);
    if (FAILED(device.SetDataFormat(&format)) ||
        FAILED(device.SetCooperativeLevel(window, cooperation))) {
        throw std::runtime_error{"DirectInput device configuration failed"};
    }
}

class hidden_window final {
public:
    hidden_window() {
        static constexpr auto class_name =
            std::to_array(L"InputNext.DirectInput.Integration.Window");
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name.data();
        if (RegisterClassW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error{"failed to register test window class"};
        }
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class_name.data(),
            L"InputNext DirectInput test window",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error{"failed to create hidden test window"};
        }
    }

    ~hidden_window() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    hidden_window(const hidden_window&) = delete;
    hidden_window& operator=(const hidden_window&) = delete;
    hidden_window(hidden_window&&) = delete;
    hidden_window& operator=(hidden_window&&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

    [[nodiscard]] focus::window_id id() const noexcept {
        return reinterpret_cast<focus::window_id>(window_);
    }

private:
    HWND window_{};
};

class switchable_window_source final : public focus::source {
public:
    explicit switchable_window_source(focus::process_instance process) noexcept
        : process_(process),
          reading_{.process = process,
              .state = focus::evidence::foreground,
              .foreground_window = 1U,
              .window_transition = 1U,
              .window_available = true} {}

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        std::unique_lock lock{gate_};
        ++calls_;
        if (block_next_) {
            block_next_ = false;
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
            blocked_ = false;
            released_ = false;
        }
        const auto result = reading_;
        changed_.notify_all();
        return result;
    }

    void set(const focus::reading& reading) noexcept {
        std::scoped_lock lock{gate_};
        reading_ = reading;
        changed_.notify_all();
    }

    void block_next_observation() noexcept {
        std::scoped_lock lock{gate_};
        block_next_ = true;
        blocked_ = false;
        released_ = false;
    }

    [[nodiscard]] bool wait_until_blocked(std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this] { return blocked_; });
    }

    void release_blocked_observation() noexcept {
        std::scoped_lock lock{gate_};
        released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] std::uint64_t calls() const noexcept {
        std::scoped_lock lock{gate_};
        return calls_;
    }

private:
    const focus::process_instance process_;
    mutable std::mutex gate_;
    std::condition_variable changed_;
    focus::reading reading_{};
    std::uint64_t calls_{};
    bool block_next_{};
    bool blocked_{};
    bool released_{};
};

class focus_window_fixture final {
public:
    using clock_callback = std::chrono::steady_clock::time_point (*)(const void*) noexcept;

    explicit focus_window_fixture(const clock_callback now = nullptr,
        const void* clock_context = nullptr) {
        auto source = std::make_unique<switchable_window_source>(process_);
        source_ = source.get();
        if (now != nullptr && clock_context == nullptr) {
            throw std::invalid_argument{"custom focus clock requires a context"};
        }
        observer_ = focus::detail::observer_test_access::make_with_clock(focus_identity(),
            std::move(source),
            now == nullptr ? &test::focus::manual_clock::read : now,
            now == nullptr ? static_cast<const void*>(&clock_) : clock_context);
        if (!wait_until([this] {
                const auto published = observer_->published();
                return published.window_available && published.foreground_window == 1U;
            })) {
            throw std::runtime_error{"window focus test observer failed to start"};
        }
    }

    ~focus_window_fixture() {
        source_->release_blocked_observation();
        if (observer_) {
            observer_->stop();
        }
    }

    focus_window_fixture(const focus_window_fixture&) = delete;
    focus_window_fixture& operator=(const focus_window_fixture&) = delete;
    focus_window_fixture(focus_window_fixture&&) = delete;
    focus_window_fixture& operator=(focus_window_fixture&&) = delete;

    [[nodiscard]] focus::observer& observer() noexcept { return *observer_; }

    [[nodiscard]] switchable_window_source& source() noexcept { return *source_; }

    [[nodiscard]] test::focus::manual_clock& clock() noexcept { return clock_; }

    void set_window(focus::window_id window, std::uint64_t transition) noexcept {
        source_->set(focus::reading{.process = process_,
            .state = focus::evidence::foreground,
            .foreground_window = window,
            .window_transition = transition,
            .window_available = true});
    }

    void
    set_foreground(bool foreground, focus::window_id window, std::uint64_t transition) noexcept {
        source_->set(focus::reading{.process = process_,
            .state = foreground ? focus::evidence::foreground : focus::evidence::background,
            .foreground_window = window,
            .window_transition = transition,
            .window_available = foreground});
    }

    [[nodiscard]] bool wait_for_window(focus::window_id window,
        std::uint64_t transition,
        std::chrono::milliseconds timeout = 2s) const {
        return wait_until(
            [this, window, transition] {
                const auto published = observer_->published();
                return published.window_available && published.foreground_window == window &&
                       published.window_transition == transition;
            },
            timeout);
    }

    [[nodiscard]] bool wait_for_evidence(focus::evidence state,
        std::chrono::milliseconds timeout = 2s) const {
        return wait_until([this, state] { return observer_->published().state == state; }, timeout);
    }

    [[nodiscard]] bool wait_until_blocked(std::chrono::milliseconds timeout = 2s) {
        return source_->wait_until_blocked(timeout);
    }

private:
    template <typename Predicate>
    [[nodiscard]] bool wait_until(Predicate predicate,
        std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    [[nodiscard]] static runtime_id focus_identity() noexcept {
        auto identity = gameinput_test_support::test_runtime_id();
        identity.bytes[0U] = std::byte{0x91};
        return identity;
    }

    const focus::process_instance process_{.pid = GetCurrentProcessId(),
        .creation_time_100ns = 0x912345U};
    test::focus::manual_clock clock_;
    switchable_window_source* source_{};
    std::unique_ptr<focus::observer> observer_;
};

[[nodiscard]] inline std::shared_ptr<dinput::input_access>
make_access(core_test_support::fixture& owners, focus::observer& focus_observer) {
    return std::make_shared<dinput::input_access>(*owners.core,
        *owners.gameinput.runtime,
        focus_observer);
}

[[nodiscard]] inline bool wait_for_sample(core::service& core_service,
    connection_target target,
    const gamepad_state& expected,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto latest = core_service.read_latest(target);
        if (latest.has_value() && latest->sample.state == expected) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

inline void publish_gamepad(core_test_support::fixture& owners,
    std::size_t index,
    const gamepad_state& state,
    std::uint64_t timestamp = 121U) {
    owners.gameinput.native->publish(
        gameinput_test_support::handle(owners.gameinput.device_memory[index]),
        gameinput_test_support::pad_parts(state, timestamp, timestamp * 10U));
}

} // namespace inputnext::dinput_test_support
