#include "catalogue_test_support.hpp"
#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/com/device.hpp"
#include "dinput/com/factory.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/device_info.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/native_types.hpp"
#include "dinput/property_abi.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view device_path{R"(\\?\HID#VID_054C&PID_0CE6#ExclusiveDevice)"};
constexpr UINT exclusive_callback_message = WM_APP + 0x48U;

struct root_context final {
    std::weak_ptr<dinput::input_access> access;
    std::shared_ptr<system_input_state> system;
};

std::shared_ptr<dinput::input_access> root_session(void* value) noexcept {
    return static_cast<root_context*>(value)->access.lock();
}

HRESULT create_root_native(void* value,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** output,
    IUnknown* outer) noexcept {
    host_context native{};
    native.system = static_cast<root_context*>(value)->system;
    return create_system(&native, instance, version, iid, output, outer);
}

class exclusive_fixture final {
public:
    exclusive_fixture() {
        focus.set_window(window.id(), 2U);
        if (!focus.wait_for_window(window.id(), 2U)) {
            throw std::runtime_error{"foreground observation unavailable"};
        }
        catalogue.context.access = make_access(*catalogue.owners, focus.observer());
        catalogue.add_system_gamepad(guid(1U),
            std::wstring{device_path.begin(), device_path.end()});
        catalogue.system->controls = std::make_shared<native_control_script>();
        target = catalogue.connect(0U, std::string{device_path});
        if (!catalogue.wait_for_catalogue(1U)) {
            throw std::runtime_error{"input catalogue unavailable"};
        }
        provider = std::make_shared<host_context>();
        provider->system = catalogue.system;
        auto identity = identity_for(target);
        identity.instance.guidInstance = guid(1U);
        DIPROPGUIDANDPATH path{};
        path.diph.dwSize = data_size<DIPROPGUIDANDPATH>();
        path.diph.dwHeaderSize = data_size<DIPROPHEADER>();
        path.diph.dwHow = DIPH_DEVICE;
        path.guidClass = GUID_Joystick;
        const std::wstring wide_path{device_path.begin(), device_path.end()};
        wcscpy_s(path.wszPath, wide_path.c_str());
        identity.path = path;
        const dinput::native_input_factory factory{.create = &create_system,
            .context = provider.get(),
            .instance = GetModuleHandleW(nullptr),
            .lifetime = provider};
        void* raw{};
        if (FAILED(dinput::make_device(identity,
                catalogue.context.access,
                parent,
                true,
                &raw,
                factory))) {
            throw std::runtime_error{"normalized device creation failed"};
        }
        device = com_owner<IDirectInputDevice8W>{static_cast<IDirectInputDevice8W*>(raw)};
        configure_compact_device(*device, window.get(), DISCL_FOREGROUND | DISCL_EXCLUSIVE);
    }

    ~exclusive_fixture() {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(native_operation::count);
            ++index) {
            catalogue.system->controls->release_block(static_cast<native_operation>(index));
        }
        catalogue.system->controls->clear_send_message();
        device.reset();
        catalogue.context.access->close_admission();
        catalogue.context.access->drain();
    }

    exclusive_fixture(const exclusive_fixture&) = delete;
    exclusive_fixture& operator=(const exclusive_fixture&) = delete;
    exclusive_fixture(exclusive_fixture&&) = delete;
    exclusive_fixture& operator=(exclusive_fixture&&) = delete;

    hidden_window window;
    focus_window_fixture focus;
    environment catalogue;
    std::shared_ptr<host_context> provider;
    parent_unknown parent;
    connection_target target{};
    com_owner<IDirectInputDevice8W> device;
};

class unblock_native final {
public:
    unblock_native(std::shared_ptr<native_control_script> script, native_operation operation)
        : script_(std::move(script)),
          operation_(operation) {}

    ~unblock_native() { script_->release_block(operation_); }

    unblock_native(const unblock_native&) = delete;
    unblock_native& operator=(const unblock_native&) = delete;
    unblock_native(unblock_native&&) = delete;
    unblock_native& operator=(unblock_native&&) = delete;

private:
    const std::shared_ptr<native_control_script> script_;
    const native_operation operation_;
};

class event_handle final {
public:
    explicit event_handle(const bool signalled = false) noexcept
        : value_{CreateEventW(nullptr, TRUE, signalled ? TRUE : FALSE, nullptr)} {}

    ~event_handle() {
        if (value_ != nullptr) {
            static_cast<void>(CloseHandle(value_));
        }
    }

    event_handle(const event_handle&) = delete;
    event_handle& operator=(const event_handle&) = delete;
    event_handle(event_handle&&) = delete;
    event_handle& operator=(event_handle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HANDLE value_{};
};

struct callback_context final {
    std::function<void()> action{nullptr};
    HANDLE completed{};
    std::uint32_t calls{};
    bool failed{};
};

[[nodiscard]] LRESULT CALLBACK exclusive_callback_window_proc(HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) noexcept {
    auto* context = std::bit_cast<callback_context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = std::bit_cast<const CREATESTRUCTW*>(lparam);
        context = static_cast<callback_context*>(creation->lpCreateParams);
        static_cast<void>(
            SetWindowLongPtrW(window, GWLP_USERDATA, std::bit_cast<LONG_PTR>(context)));
    }
    if (context != nullptr && message == exclusive_callback_message) {
        ++context->calls;
        try {
            if (context->action) {
                context->action();
            }
        } catch (...) {
            context->failed = true;
        }
        if (context->completed != nullptr) {
            static_cast<void>(SetEvent(context->completed));
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void pump_sent_until(HANDLE completed, HWND window) noexcept {
    while (WaitForSingleObject(completed, 0U) != WAIT_OBJECT_0) {
        MSG message{};
        static_cast<void>(PeekMessageW(&message, window, WM_NULL, WM_NULL, PM_NOREMOVE));
        static_cast<void>(WaitForSingleObject(completed, 1U));
    }
}

class callback_window final {
public:
    explicit callback_window(callback_context& context) {
        static constexpr auto class_name =
            std::to_array(L"InputNext.DirectInput.ExclusiveDeviceTests");
        WNDCLASSEXW window_class{};
        window_class.cbSize = static_cast<UINT>(sizeof(window_class));
        window_class.lpfnWndProc = &exclusive_callback_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name.data();
        if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error{"failed to register exclusive device callback window"};
        }

        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            class_name.data(),
            L"InputNext exclusive device callback window",
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
            throw std::runtime_error{"failed to create exclusive device callback window"};
        }
    }

    ~callback_window() {
        if (window_ != nullptr) {
            static_cast<void>(DestroyWindow(window_));
        }
    }

    callback_window(const callback_window&) = delete;
    callback_window& operator=(const callback_window&) = delete;
    callback_window(callback_window&&) = delete;
    callback_window& operator=(callback_window&&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

private:
    HWND window_{};
};

class history_message_sender final {
public:
    history_message_sender(std::shared_ptr<gameinput_test_support::native_history_script> history,
        std::shared_ptr<native_control_script> controls,
        const HWND window,
        const HANDLE acquire_completed)
        : window_{window} {
        if (!finished_) {
            throw std::runtime_error{"failed to create history message sender event"};
        }
        const HANDLE finished_event = finished_.get();
        sender_ = std::thread{[this,
                                  history = std::move(history),
                                  controls = std::move(controls),
                                  window,
                                  acquire_completed,
                                  finished_event] {
            if (history->wait_until_blocked(5s)) {
                history_blocked_.store(true, std::memory_order_release);
                controls->block_next(native_operation::unacquire, true);
                static_cast<void>(SendMessageW(window, exclusive_callback_message, 0U, 0));
                if (WaitForSingleObject(acquire_completed, 5'000U) != WAIT_OBJECT_0) {
                    history->release_blocked_call();
                }
            } else {
                // If acquisition never reaches history readiness, let any later
                // history call pass so assertion unwinding cannot strand Acquire.
                history->release_blocked_call();
            }
            static_cast<void>(SetEvent(finished_event));
        }};
    }

    ~history_message_sender() { finish(); }

    history_message_sender(const history_message_sender&) = delete;
    history_message_sender& operator=(const history_message_sender&) = delete;
    history_message_sender(history_message_sender&&) = delete;
    history_message_sender& operator=(history_message_sender&&) = delete;

    [[nodiscard]] bool history_was_blocked() const noexcept {
        return history_blocked_.load(std::memory_order_acquire);
    }

    void finish() noexcept {
        if (!sender_.joinable()) {
            return;
        }
        pump_sent_until(finished_.get(), window_);
        sender_.join();
    }

private:
    HWND window_{};
    event_handle finished_;
    std::atomic<bool> history_blocked_{false};
    std::thread sender_;
};

[[nodiscard]] com_owner<IDirectInputDevice8W> make_secondary_exclusive_device(
    exclusive_fixture& fixture,
    const connection_target target,
    const GUID native_guid,
    const std::wstring_view native_path) {
    auto identity = identity_for(target);
    identity.instance.guidInstance = native_guid;
    DIPROPGUIDANDPATH path{};
    path.diph.dwSize = data_size<DIPROPGUIDANDPATH>();
    path.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    path.diph.dwHow = DIPH_DEVICE;
    path.guidClass = GUID_Joystick;
    const std::wstring owned_path{native_path};
    if (wcscpy_s(path.wszPath, owned_path.c_str()) != 0) {
        throw std::runtime_error{"secondary exclusive native path is too long"};
    }
    identity.path = path;
    const dinput::native_input_factory factory{.create = &create_system,
        .context = fixture.provider.get(),
        .instance = GetModuleHandleW(nullptr),
        .lifetime = fixture.provider};
    void* raw{};
    if (FAILED(dinput::make_device(identity,
            fixture.catalogue.context.access,
            fixture.parent,
            true,
            &raw,
            factory))) {
        throw std::runtime_error{"secondary exclusive device creation failed"};
    }
    com_owner<IDirectInputDevice8W> result{static_cast<IDirectInputDevice8W*>(raw)};
    configure_compact_device(*result, fixture.window.get(), DISCL_FOREGROUND | DISCL_EXCLUSIVE);
    return result;
}

} // namespace

TEST_CASE("dinput.exclusive_root_propagates_owned_factory_to_both_device_views",
    "[integration][dinput][exclusive][com]") {
    const exclusive_fixture fixture;
    const auto context = std::make_shared<root_context>();
    context->access = fixture.catalogue.context.access;
    context->system = fixture.catalogue.system;
    const dinput::host host{.session = &root_session,
        .context = context.get(),
        .create_system = &create_root_native,
        .system_lifetime = context};
    void* value{};
    REQUIRE(dinput::create_root(host,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                &value,
                nullptr) == DI_OK);
    com_owner<IDirectInput8A> root{static_cast<IDirectInput8A*>(value)};
    IDirectInputDevice8A* raw{};
    REQUIRE(root->CreateDevice(guid(1U), &raw, nullptr) == DI_OK);
    const com_owner<IDirectInputDevice8A> ansi{raw};
    void* wide_value{};
    REQUIRE(ansi->QueryInterface(IID_IDirectInputDevice8W, &wide_value) == S_OK);
    const com_owner<IDirectInputDevice8W> wide{static_cast<IDirectInputDevice8W*>(wide_value)};
    configure_compact_device(*wide, fixture.window.get(), DISCL_FOREGROUND | DISCL_EXCLUSIVE);
    root.reset();
    REQUIRE(ansi->Acquire() == DI_OK);
    compact_state state{};
    CHECK(wide->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(ansi->Unacquire() == DI_OK);
}

TEST_CASE("dinput.exclusive_device_keeps_immediate_input_on_the_common_cache",
    "[integration][dinput][exclusive][cache]") {
    exclusive_fixture fixture;
    const auto expected = sample_state();
    publish_gamepad(*fixture.catalogue.owners, 0U, expected);
    REQUIRE(wait_for_sample(*fixture.catalogue.owners->core, fixture.target, expected));
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(fixture.device->Acquire() == S_FALSE);
    fixture.catalogue.owners->assign_xinput(fixture.target);
    XINPUT_STATE xinput_state{};
    REQUIRE(fixture.catalogue.owners->presentation->get_state(xinput::flavor::v1_4,
                0U,
                &xinput_state) == ERROR_SUCCESS);
    fixture.catalogue.owners->presentation->enable(FALSE);
    REQUIRE(fixture.catalogue.owners->presentation->get_state(xinput::flavor::v1_4,
                0U,
                &xinput_state) == ERROR_SUCCESS);
    CHECK(xinput_state.Gamepad.wButtons == 0U);
    const auto latest = fixture.catalogue.owners->core->read_latest(fixture.target);
    REQUIRE(latest.has_value());
    CHECK(latest->sample.state == expected);
    const auto facts_calls = fixture.catalogue.owners->gameinput.native->facts_calls;
    const auto current_calls = fixture.catalogue.owners->gameinput.native->current_calls;
    const auto history_factories = fixture.catalogue.owners->scripts[0U]->cursor_factories.load();
    compact_state state{};
    REQUIRE(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(state.buttons[0U] == 0x80U);
    CHECK(fixture.device->Poll() == DI_NOEFFECT);
    CHECK(fixture.catalogue.owners->gameinput.native->facts_calls == facts_calls);
    CHECK(fixture.catalogue.owners->gameinput.native->current_calls == current_calls);
    CHECK(fixture.catalogue.owners->scripts[0U]->cursor_factories.load() == history_factories);
    REQUIRE(fixture.device->Unacquire() == DI_OK);
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_NOTACQUIRED);
    CHECK(fixture.device->Unacquire() == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.exclusive_device_loses_fast_window_transitions_until_explicit_acquire",
    "[integration][dinput][exclusive][focus]") {
    exclusive_fixture fixture;
    const hidden_window other_window;
    REQUIRE(fixture.device->Acquire() == DI_OK);
    fixture.focus.set_window(other_window.id(), 3U);
    REQUIRE(fixture.focus.wait_for_window(other_window.id(), 3U));
    fixture.focus.set_window(fixture.window.id(), 4U);
    REQUIRE(fixture.focus.wait_for_window(fixture.window.id(), 4U));
    auto state = sentinel_state(0x5aU);
    const auto before = state;
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == before);
    REQUIRE(fixture.device->Unacquire() == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.exclusive_device_buffered_acquisition_uses_existing_history_and_queue",
    "[integration][dinput][exclusive][buffered]") {
    exclusive_fixture fixture;
    DIPROPDWORD size{};
    size.diph.dwSize = data_size<DIPROPDWORD>();
    size.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    size.diph.dwHow = DIPH_DEVICE;
    size.dwData = 8U;
    REQUIRE(inputnext_dinput_set_property_w(fixture.device.get(), &DIPROP_BUFFERSIZE, &size.diph) ==
            DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    DIDEVICEOBJECTDATA event{};
    DWORD count = 1U;
    const auto result =
        fixture.device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), &event, &count, 0U);
    CHECK((result == DI_OK || result == DI_BUFFEROVERFLOW));
    CHECK(count == 0U);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.immediate_reacquire_discards_the_lost_buffered_subscription",
    "[integration][dinput][exclusive][buffered][focus]") {
    exclusive_fixture fixture;
    SECTION("previous exclusive acquisition") {
    }
    SECTION("previous nonexclusive acquisition") {
        REQUIRE(fixture.device->SetCooperativeLevel(fixture.window.get(),
                    DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    }
    prepare_buffered_history(*fixture.catalogue.owners);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);

    const hidden_window other_window;
    fixture.focus.set_window(other_window.id(), 3U);
    REQUIRE(fixture.focus.wait_for_window(other_window.id(), 3U));
    REQUIRE(fixture.device->Poll() == DIERR_INPUTLOST);
    REQUIRE(set_buffer_size(*fixture.device, 0U) == DI_OK);
    REQUIRE(fixture.device->SetCooperativeLevel(fixture.window.get(),
                DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    compact_state state{};
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(fixture.device->Poll() == DI_NOEFFECT);
}

TEST_CASE(
    "dinput.exclusive_device_cancels_a_blocked_native_acquire_without_publishing_late_success",
    "[integration][dinput][exclusive][concurrency]") {
    exclusive_fixture fixture;
    const auto script = fixture.catalogue.system->controls;
    script->block_next(native_operation::acquire);
    auto pending = std::async(std::launch::async, [&fixture] { return fixture.device->Acquire(); });
    const unblock_native cleanup{script, native_operation::acquire};
    REQUIRE(script->wait_for_entry(native_operation::acquire));
    CHECK(fixture.device->Unacquire() == DI_OK);
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    CHECK(pending.get() == DIERR_INPUTLOST);
    auto state = sentinel_state(0x33U);
    const auto before = state;
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_NOTACQUIRED);
    CHECK(state == before);
    CHECK(fixture.device->Acquire() == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(script->count(native_operation::acquire) == 1U);
    script->release_block(native_operation::acquire);
    REQUIRE(fixture.device->Unacquire() == DI_OK);
    CHECK(script->count(native_operation::unacquire) == 1U);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.exclusive_device_keeps_local_reads_prompt_while_native_unacquire_is_blocked",
    "[integration][dinput][exclusive][concurrency]") {
    exclusive_fixture fixture;
    const auto script = fixture.catalogue.system->controls;
    REQUIRE(fixture.device->Acquire() == DI_OK);
    script->block_next(native_operation::unacquire);
    auto closing =
        std::async(std::launch::async, [&fixture] { return fixture.device->Unacquire(); });
    const unblock_native cleanup{script, native_operation::unacquire};
    REQUIRE(script->wait_for_entry(native_operation::unacquire));
    compact_state state{};
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_NOTACQUIRED);
    CHECK(fixture.device->Poll() == DIERR_NOTACQUIRED);
    CHECK(closing.wait_for(0ms) == std::future_status::timeout);
    CHECK(fixture.device->Acquire() == HRESULT_FROM_WIN32(ERROR_BUSY));
    script->release_block(native_operation::unacquire);
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    CHECK(closing.get() == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
}

TEST_CASE("dinput.exclusive_devices_are_independent_of_the_four_xinput_positions",
    "[integration][dinput][exclusive][coexistence]") {
    exclusive_fixture fixture;
    std::array<connection_target, 4U> targets{};
    std::array<std::wstring, 4U> paths{};
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        const auto path = std::string{device_path} + std::to_string(index + 1U);
        paths[index] = std::wstring{path.begin(), path.end()};
        fixture.catalogue.add_system_gamepad(guid(static_cast<std::uint32_t>(index + 2U)),
            paths[index]);
        targets[index] = fixture.catalogue.connect(index + 1U, path);
    }
    REQUIRE(fixture.catalogue.wait_for_catalogue(5U));
    std::array<com_owner<IDirectInputDevice8W>, 4U> devices;
    const dinput::native_input_factory factory{.create = &create_system,
        .context = fixture.provider.get(),
        .instance = GetModuleHandleW(nullptr),
        .lifetime = fixture.provider};
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        auto identity = identity_for(targets[index]);
        identity.instance.guidInstance = guid(static_cast<std::uint32_t>(index + 2U));
        DIPROPGUIDANDPATH path{};
        path.diph.dwSize = data_size<DIPROPGUIDANDPATH>();
        path.diph.dwHeaderSize = data_size<DIPROPHEADER>();
        path.diph.dwHow = DIPH_DEVICE;
        path.guidClass = GUID_Joystick;
        wcscpy_s(path.wszPath, paths[index].c_str());
        identity.path = path;
        void* raw{};
        REQUIRE(dinput::make_device(identity,
                    fixture.catalogue.context.access,
                    fixture.parent,
                    true,
                    &raw,
                    factory) == DI_OK);
        devices[index] = com_owner<IDirectInputDevice8W>{static_cast<IDirectInputDevice8W*>(raw)};
        configure_compact_device(*devices[index],
            fixture.window.get(),
            DISCL_FOREGROUND | DISCL_EXCLUSIVE);
    }
    REQUIRE(fixture.device->Acquire() == DI_OK);
    for (const auto& device : devices) {
        REQUIRE(device->Acquire() == DI_OK);
        compact_state state{};
        CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    }
}

TEST_CASE("dinput.exclusive_acquire_sent_callback_can_release_its_last_external_com_reference",
    "[integration][dinput][exclusive][com][reentrancy]") {
    exclusive_fixture fixture;
    const event_handle callback_completed;
    REQUIRE(static_cast<bool>(callback_completed));

    HRESULT cancellation = E_UNEXPECTED;
    ULONG parent_references_after_release{};
    callback_context callback{.completed = callback_completed.get()};
    callback.action = [&] {
        cancellation = fixture.device->Unacquire();
        fixture.device.reset();
        parent_references_after_release = fixture.parent.references();
    };
    const callback_window window{callback};
    const auto controls = fixture.catalogue.system->controls;
    controls->send_message_on(native_operation::acquire,
        native_message{.window = window.get(), .message = exclusive_callback_message});

    const HRESULT acquisition = fixture.device->Acquire();

    CHECK(callback.calls == 1U);
    CHECK(!callback.failed);
    CHECK(cancellation == DI_OK);
    CHECK(parent_references_after_release == 2U);
    CHECK(!static_cast<bool>(fixture.device));
    CHECK(acquisition == DIERR_INPUTLOST);
    CHECK(fixture.parent.references() == 1U);
    fixture.catalogue.context.access->close_admission();
    fixture.catalogue.context.access->drain();
    CHECK(controls->count(native_operation::acquire) == 1U);
    CHECK(controls->count(native_operation::unacquire) == 1U);
    CHECK(controls->count(native_operation::release_device) ==
          controls->count(native_operation::create_device));
    CHECK(controls->count(native_operation::release_root) == 1U);
}

TEST_CASE("dinput.exclusive_acquire_callbacks_reject_same_and_cross_device_reentry",
    "[integration][dinput][exclusive][com][reentrancy]") {
    exclusive_fixture fixture;
    const std::string secondary_path{R"(\\?\HID#VID_054C&PID_0CE6#ExclusiveDeviceTwo)"};
    const std::wstring secondary_native_path{secondary_path.begin(), secondary_path.end()};
    fixture.catalogue.add_system_gamepad(guid(2U), secondary_native_path);
    const auto secondary_target = fixture.catalogue.connect(1U, secondary_path);
    REQUIRE(fixture.catalogue.wait_for_catalogue(2U));
    const auto secondary =
        make_secondary_exclusive_device(fixture, secondary_target, guid(2U), secondary_native_path);
    const event_handle callback_completed;
    REQUIRE(static_cast<bool>(callback_completed));

    HRESULT same_device = E_UNEXPECTED;
    HRESULT other_device = E_UNEXPECTED;
    callback_context callback{.completed = callback_completed.get()};
    callback.action = [&] {
        same_device = fixture.device->Acquire();
        other_device = secondary->Acquire();
    };
    const callback_window window{callback};
    const auto controls = fixture.catalogue.system->controls;
    controls->send_message_on(native_operation::acquire,
        native_message{.window = window.get(), .message = exclusive_callback_message});

    const HRESULT acquisition = fixture.device->Acquire();

    CHECK(callback.calls == 1U);
    CHECK(!callback.failed);
    CHECK(same_device == DIERR_ACQUIRED);
    CHECK(other_device == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(acquisition == DI_OK);
    CHECK(controls->count(native_operation::factory_create) == 1U);
    CHECK(controls->count(native_operation::acquire) == 1U);
    compact_state secondary_state{};
    CHECK(secondary->GetDeviceState(data_size<compact_state>(), &secondary_state) ==
          DIERR_NOTACQUIRED);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.exclusive_sent_unacquire_returns_pending_then_ordinary_calls_wait_for_teardown",
    "[integration][dinput][exclusive][com][reentrancy][concurrency]") {
    exclusive_fixture fixture;
    REQUIRE(fixture.device->Acquire() == DI_OK);
    const auto controls = fixture.catalogue.system->controls;
    controls->block_next(native_operation::unacquire, true);
    const event_handle callback_completed;
    REQUIRE(static_cast<bool>(callback_completed));

    HRESULT reentrant_unacquire = E_UNEXPECTED;
    HRESULT reentrant_acquire = E_UNEXPECTED;
    callback_context callback{.completed = callback_completed.get()};
    callback.action = [&] {
        reentrant_unacquire = fixture.device->Unacquire();
        reentrant_acquire = fixture.device->Acquire();
    };
    const callback_window window{callback};
    controls->send_message_on(native_operation::unacquire,
        native_message{.window = window.get(), .message = exclusive_callback_message});

    const event_handle closing_completed;
    REQUIRE(static_cast<bool>(closing_completed));
    auto closing = std::async(std::launch::async, [&fixture, completed = closing_completed.get()] {
        const HRESULT result = fixture.device->Unacquire();
        static_cast<void>(SetEvent(completed));
        return result;
    });
    const unblock_native cleanup{controls, native_operation::unacquire};
    const bool native_entered = controls->wait_for_entry(native_operation::unacquire);
    const std::array completion_events{callback_completed.get(), closing_completed.get()};
    std::size_t completion_index{};
    const HRESULT dispatched = dinput::wait_for_native_control(completion_events, completion_index);
    if (dispatched != DI_OK) {
        controls->release_block(native_operation::unacquire);
        pump_sent_until(closing_completed.get(), window.get());
    }

    CHECK(dispatched == DI_OK);
    CHECK(native_entered);
    CHECK(completion_index == 0U);
    CHECK(callback.calls == 1U);
    CHECK(!callback.failed);
    CHECK(reentrant_unacquire == E_PENDING);
    CHECK(reentrant_acquire == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(closing.wait_for(0ms) == std::future_status::timeout);
    CHECK(fixture.device->Acquire() == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(controls->count(native_operation::factory_create) == 1U);

    std::promise<void> later_started;
    const auto later_started_future = later_started.get_future();
    auto later = std::async(std::launch::async, [&fixture, &later_started] {
        later_started.set_value();
        return fixture.device->Unacquire();
    });
    const unblock_native later_cleanup{controls, native_operation::unacquire};
    later_started_future.wait();
    CHECK(later.wait_for(20ms) == std::future_status::timeout);

    controls->release_block(native_operation::unacquire);
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    REQUIRE(later.wait_for(2s) == std::future_status::ready);
    CHECK(closing.get() == DI_OK);
    CHECK(later.get() == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(controls->count(native_operation::factory_create) == 2U);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE(
    "dinput.exclusive_buffered_acquire_cancellation_wakes_history_wait_and_keeps_native_lease",
    "[integration][dinput][exclusive][buffered][cancellation][reentrancy]") {
    exclusive_fixture fixture;
    prepare_buffered_history(*fixture.catalogue.owners, 0U);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);
    const auto history = fixture.catalogue.owners->scripts[0U];
    history->block_next();
    core_test_support::history_call_release_guard history_cleanup{history};
    const auto controls = fixture.catalogue.system->controls;
    const unblock_native native_cleanup{controls, native_operation::unacquire};
    const event_handle callback_completed;
    REQUIRE(static_cast<bool>(callback_completed));

    HRESULT cancellation = E_UNEXPECTED;
    bool reentrant{};
    callback_context callback{.completed = callback_completed.get()};
    callback.action = [&] {
        reentrant = dinput::native_control_reentrant();
        cancellation = fixture.device->Unacquire();
    };
    const callback_window window{callback};
    const event_handle acquire_completed;
    REQUIRE(static_cast<bool>(acquire_completed));
    history_message_sender sender{history, controls, window.get(), acquire_completed.get()};

    const HRESULT acquisition = fixture.device->Acquire();
    static_cast<void>(SetEvent(acquire_completed.get()));
    sender.finish();

    CHECK(sender.history_was_blocked());
    CHECK(callback.calls == 1U);
    CHECK(!callback.failed);
    CHECK(reentrant);
    CHECK(cancellation == DI_OK);
    CHECK(WaitForSingleObject(callback_completed.get(), 0U) == WAIT_OBJECT_0);
    CHECK(acquisition == DIERR_INPUTLOST);
    REQUIRE(controls->wait_for_entry(native_operation::unacquire));
    CHECK(fixture.device->Acquire() == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(controls->count(native_operation::factory_create) == 1U);

    std::promise<void> later_started;
    const auto later_started_future = later_started.get_future();
    auto later = std::async(std::launch::async, [&fixture, &later_started] {
        later_started.set_value();
        return fixture.device->Unacquire();
    });
    const unblock_native later_cleanup{controls, native_operation::unacquire};
    later_started_future.wait();
    CHECK(later.wait_for(20ms) == std::future_status::timeout);

    controls->release_block(native_operation::unacquire);
    history_cleanup.release();
    REQUIRE(later.wait_for(2s) == std::future_status::ready);
    CHECK(later.get() == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(controls->count(native_operation::factory_create) == 2U);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.exclusive_close_admission_wakes_buffered_wait_and_drains_blocked_owners",
    "[integration][dinput][exclusive][buffered][shutdown]") {
    exclusive_fixture fixture;
    prepare_buffered_history(*fixture.catalogue.owners, 0U);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);
    const auto history = fixture.catalogue.owners->scripts[0U];
    history->block_next();
    core_test_support::history_call_release_guard history_cleanup{history};
    const auto controls = fixture.catalogue.system->controls;
    const unblock_native native_cleanup{controls, native_operation::unacquire};
    const event_handle callback_completed;
    REQUIRE(static_cast<bool>(callback_completed));

    callback_context callback{.completed = callback_completed.get()};
    callback.action = [&] {
        fixture.catalogue.context.access->close_admission();
    };
    const callback_window window{callback};
    const event_handle acquire_completed;
    REQUIRE(static_cast<bool>(acquire_completed));
    history_message_sender sender{history, controls, window.get(), acquire_completed.get()};

    const HRESULT acquisition = fixture.device->Acquire();
    static_cast<void>(SetEvent(acquire_completed.get()));
    sender.finish();

    CHECK(sender.history_was_blocked());
    CHECK(callback.calls == 1U);
    CHECK(!callback.failed);
    CHECK(WaitForSingleObject(callback_completed.get(), 0U) == WAIT_OBJECT_0);
    CHECK(acquisition == DIERR_INPUTLOST);
    CHECK(!fixture.catalogue.context.access->open());
    REQUIRE(controls->wait_for_entry(native_operation::unacquire));

    const auto access = fixture.catalogue.context.access;
    auto draining = std::async(std::launch::async, [access] { access->drain(); });
    const unblock_native drain_native_cleanup{controls, native_operation::unacquire};
    const core_test_support::history_call_release_guard drain_history_cleanup{history};
    CHECK(draining.wait_for(20ms) == std::future_status::timeout);

    controls->release_block(native_operation::unacquire);
    history_cleanup.release();
    REQUIRE(draining.wait_for(2s) == std::future_status::ready);
    draining.get();
    CHECK(controls->count(native_operation::factory_create) == 1U);
    CHECK(controls->count(native_operation::unacquire) == 1U);
}
