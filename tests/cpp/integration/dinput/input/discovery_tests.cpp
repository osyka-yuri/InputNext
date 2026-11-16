#include "catalogue_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/catalogue/discovery.hpp"
#include "dinput/com/factory.hpp"
#include "dinput/control_wait.hpp"
#include "dinput/native_types.hpp"
#include "gameinput/native/native_gameinput.hpp"

#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace inputnext::dinput {

struct catalogue_discovery_test_access final {
    [[nodiscard]] static std::size_t waiter_count(const catalogue_discovery& discovery) {
        const auto owner = discovery.state_;
        std::scoped_lock lock{owner->gate};
        return owner->waiter_count;
    }
};

} // namespace inputnext::dinput

namespace inputnext::dinput_catalogue_test {
namespace {

constexpr auto* startup_path = R"(\\?\HID#VID_054C&PID_0CE6#Startup)";
constexpr auto* native_startup_path = LR"(\\?\hid#vid_054c&pid_0ce6#startup)";
using device_owner = dinput_test_support::com_owner<IDirectInputDevice8W>;

struct creation final {
    HRESULT status{E_UNEXPECTED};
    device_owner device;
};

[[nodiscard]] creation create(IDirectInput8W& root, const GUID& instance) {
    IDirectInputDevice8W* device{};
    const auto status = root.CreateDevice(instance, &device, nullptr);
    return {.status = status, .device = device_owner{device}};
}

[[nodiscard]] DWORD axes(const device_owner& device) {
    DIDEVCAPS capabilities{};
    capabilities.dwSize = sizeof(capabilities);
    if (!device || FAILED(device->GetCapabilities(&capabilities))) {
        return MAXDWORD;
    }
    return capabilities.dwAxes;
}

void observe(environment& fixture) {
    auto facts = gameinput_test_support::runtime_fixture::native_facts(u"startup");
    facts.pnp_path_utf8 =
        metadata_field<std::string>{.availability = metadata_availability::available,
            .value = startup_path};
    facts.gameinput_device_id = available_gameinput_id(1U);
    facts.gameinput_root_id = available_gameinput_id(81U);
    const auto device = gameinput_test_support::handle(fixture.owners->gameinput.device_memory[0U]);
    fixture.owners->gameinput.native->update_facts(device, std::move(facts));
    fixture.owners->gameinput.native->emit(device,
        gameinput_detail::native_device_event::connected);
}

void publish(environment& fixture) {
    fixture.owners->gameinput.maintain();
    fixture.context.access->notify_discovery();
}

void set_budget(environment& fixture, std::chrono::milliseconds budget) {
    fixture.context.access = std::make_shared<dinput::input_access>(*fixture.owners->core,
        *fixture.owners->gameinput.runtime,
        *fixture.owners->focus_observer,
        budget);
}

[[nodiscard]] bool wait_for_discovery(const environment& fixture, std::size_t count = 1U) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (dinput::catalogue_discovery_test_access::waiter_count(
                fixture.context.access->discovery()) == count) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

class close_discovery final {
public:
    explicit close_discovery(std::shared_ptr<dinput::input_access> access) noexcept
        : access_(std::move(access)) {}

    ~close_discovery() { access_->discovery().close(); }

    close_discovery(const close_discovery&) = delete;
    close_discovery& operator=(const close_discovery&) = delete;
    close_discovery(close_discovery&&) = delete;
    close_discovery& operator=(close_discovery&&) = delete;

private:
    std::shared_ptr<dinput::input_access> access_;
};

class release_native_block final {
public:
    release_native_block(std::shared_ptr<native_control_script> controls,
        native_operation operation) noexcept
        : controls_(std::move(controls)),
          operation_(operation) {}

    ~release_native_block() { release(); }

    release_native_block(const release_native_block&) = delete;
    release_native_block& operator=(const release_native_block&) = delete;
    release_native_block(release_native_block&&) = delete;
    release_native_block& operator=(release_native_block&&) = delete;

    void release() noexcept {
        if (controls_ != nullptr) {
            controls_->release_block(operation_);
            controls_.reset();
        }
    }

private:
    std::shared_ptr<native_control_script> controls_;
    native_operation operation_{};
};

struct releasing_host final {
    host_context* context{};
    root_owner caller;
    unsigned releases{};
};

[[nodiscard]] std::shared_ptr<dinput::input_access> release_caller(void* value) noexcept {
    auto& host = *static_cast<releasing_host*>(value);
    host.caller.reset();
    ++host.releases;
    return host.context->access;
}

[[nodiscard]] HRESULT create_releasing_system(void* value,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept {
    const auto& host = *static_cast<releasing_host*>(value);
    return create_system(host.context, instance, version, iid, out, outer);
}

class discovery_window final {
public:
    explicit discovery_window(std::function<void()> delivered) : delivered_(std::move(delivered)) {
        WNDCLASSW type{};
        type.lpfnWndProc = &dispatch;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"InputNext.DiscoveryCreationTests";
        if (RegisterClassW(&type) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error{"discovery window registration failed"};
        }
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            type.lpszClassName,
            L"Discovery test",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            type.hInstance,
            this);
        if (window_ == nullptr) {
            throw std::runtime_error{"discovery window creation failed"};
        }
    }

    ~discovery_window() { static_cast<void>(DestroyWindow(window_)); }

    discovery_window(const discovery_window&) = delete;
    discovery_window& operator=(const discovery_window&) = delete;
    discovery_window(discovery_window&&) = delete;
    discovery_window& operator=(discovery_window&&) = delete;

    [[nodiscard]] HWND get() const noexcept { return window_; }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

    static constexpr UINT notification = WM_APP + 0x73U;

private:
    [[nodiscard]] static LRESULT CALLBACK dispatch(HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept {
        auto* owner = std::bit_cast<discovery_window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* setup = std::bit_cast<const CREATESTRUCTW*>(lparam);
            owner = static_cast<discovery_window*>(setup->lpCreateParams);
            static_cast<void>(
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner)));
        }
        if (message == notification && owner != nullptr) {
            try {
                owner->delivered_();
            } catch (...) {
                owner->failed_ = true;
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    std::function<void()> delivered_;
    HWND window_{};
    bool failed_{};
};

} // namespace

TEST_CASE("dinput.discovery.observed_device_is_selected_after_admission_during_creation",
    "[integration][dinput][input][discovery][concurrency]") {
    environment fixture;
    const auto instance = guid(701U);
    fixture.add_system_gamepad(instance, native_startup_path);
    observe(fixture);
    const auto before = fixture.owners->core->enumerate();
    REQUIRE(before.has_value());
    REQUIRE(before->count == 0U);
    const auto root = fixture.create_root();
    auto pending = std::async(std::launch::async, [&] { return create(*root, instance); });
    const close_discovery cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    CHECK(pending.wait_for(0ms) == std::future_status::timeout);
    publish(fixture);
    const auto admitted = fixture.owners->core->enumerate();
    REQUIRE(admitted.has_value());
    REQUIRE(admitted->count == 1U);
    const auto result = pending.get();
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 6U);
}

TEST_CASE("dinput.discovery.callback_arriving_during_creation_is_not_missed",
    "[integration][dinput][input][discovery][concurrency]") {
    environment fixture;
    const auto instance = guid(702U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();
    auto pending = std::async(std::launch::async, [&] { return create(*root, instance); });
    const close_discovery cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    observe(fixture);
    publish(fixture);
    const auto result = pending.get();
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 6U);
}

TEST_CASE("dinput.discovery.concurrent_roots_select_the_same_admitted_device",
    "[integration][dinput][input][discovery][concurrency]") {
    environment fixture;
    const auto instance = guid(703U);
    fixture.add_system_gamepad(instance, native_startup_path);
    observe(fixture);
    const auto first_root = fixture.create_root();
    const auto second_root = fixture.create_root();
    auto first = std::async(std::launch::async, [&] { return create(*first_root, instance); });
    const close_discovery first_cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    auto second = std::async(std::launch::async, [&] { return create(*second_root, instance); });
    const close_discovery second_cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture, 2U));
    publish(fixture);

    const auto first_result = first.get();
    const auto second_result = second.get();
    REQUIRE(first_result.status == DI_OK);
    REQUIRE(second_result.status == DI_OK);
    CHECK(axes(first_result.device) == 6U);
    CHECK(axes(second_result.device) == 6U);
}

TEST_CASE("dinput.discovery.capacity_forwards_excess_creations_and_recovers",
    "[integration][dinput][input][discovery][concurrency][resources]") {
    environment fixture;
    set_budget(fixture, 15s);
    const auto instance = guid(710U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();
    constexpr auto capacity = dinput::catalogue_discovery::waiter_capacity;
    std::array<std::future<creation>, capacity> admitted;
    const close_discovery cleanup{fixture.context.access};
    for (auto& request : admitted) {
        request = std::async(std::launch::async, [&] { return create(*root, instance); });
    }
    REQUIRE(wait_for_discovery(fixture, capacity));

    const auto overflow = create(*root, instance);
    REQUIRE(overflow.status == DI_OK);
    CHECK(axes(overflow.device) == 0U);
    REQUIRE(wait_for_discovery(fixture, capacity));

    observe(fixture);
    publish(fixture);
    for (auto& request : admitted) {
        const auto result = request.get();
        REQUIRE(result.status == DI_OK);
        CHECK(axes(result.device) == 6U);
    }
    REQUIRE(wait_for_discovery(fixture, 0U));
    const auto after = create(*root, instance);
    REQUIRE(after.status == DI_OK);
    CHECK(axes(after.device) == 6U);
    CHECK(axes(overflow.device) == 0U);

    const auto unmatched = guid(711U);
    fixture.add_system_gamepad(unmatched, LR"(\\?\hid#vid_1234&pid_5678#unmatched)");
    auto next_wave = std::async(std::launch::async, [&] { return create(*root, unmatched); });
    const close_discovery next_cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    fixture.context.access->discovery().close();
    const auto forwarded = next_wave.get();
    REQUIRE(forwarded.status == DI_OK);
    CHECK(axes(forwarded.device) == 0U);
}

TEST_CASE("dinput.discovery.shutdown_during_native_inspection_does_not_reenter_session_ports",
    "[integration][dinput][input][discovery][lifecycle][concurrency]") {
    environment fixture;
    const auto instance = guid(704U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();
    const auto controls = fixture.system->controls;
    controls->block_next(native_operation::enum_devices);

    auto pending = std::async(std::launch::async, [&] { return create(*root, instance); });
    release_native_block release{controls, native_operation::enum_devices};
    REQUIRE(controls->wait_for_entry(native_operation::enum_devices));
    fixture.shutdown();
    release.release();

    const auto result = pending.get();
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 0U);
    CHECK_FALSE(fixture.context.access->open());
}

TEST_CASE("dinput.discovery.exhausted_budget_keeps_old_native_objects_and_allows_new_warm_creation",
    "[integration][dinput][input][discovery][lifecycle]") {
    environment fixture;
    set_budget(fixture, 0ms);
    const auto instance = guid(705U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();

    const auto original = create(*root, instance);
    REQUIRE(original.status == DI_OK);
    REQUIRE(axes(original.device) == 0U);
    observe(fixture);
    publish(fixture);
    const auto replacement = create(*root, instance);
    REQUIRE(replacement.status == DI_OK);
    CHECK(axes(replacement.device) == 6U);
    CHECK(axes(original.device) == 0U);
}

TEST_CASE("dinput.discovery.creation_retains_parent_through_reentrant_caller_release",
    "[integration][dinput][input][discovery][com][lifecycle]") {
    environment fixture;
    const auto instance = guid(706U);
    fixture.add_system_gamepad(instance, native_startup_path);
    static_cast<void>(fixture.connect(0U, std::string{startup_path}));
    REQUIRE(fixture.wait_for_catalogue(1U));
    releasing_host host{.context = &fixture.context, .caller = {}, .releases = 0U};
    const dinput::host supplied{.session = &release_caller,
        .context = &host,
        .create_system = &create_releasing_system};
    void* value{};
    REQUIRE(dinput::create_root(supplied,
                GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                &value,
                nullptr) == DI_OK);
    host.caller = root_owner{static_cast<IDirectInput8W*>(value)};

    auto result = create(*host.caller, instance);
    REQUIRE(result.status == DI_OK);
    CHECK(host.releases == 1U);
    CHECK_FALSE(host.caller);
    CHECK(axes(result.device) == 6U);
    CHECK(dinput::can_unload() == S_FALSE);
    result.device.reset();
    CHECK(dinput::can_unload() == S_OK);
}

TEST_CASE("dinput.discovery.sent_message_can_admit_device_and_release_caller_during_wait",
    "[integration][dinput][input][discovery][com][concurrency][reentrancy]") {
    environment fixture;
    const auto instance = guid(707U);
    fixture.add_system_gamepad(instance, native_startup_path);
    auto root = fixture.create_root();
    auto* const borrowed_root = root.get();
    bool active{};
    bool during_wait{};
    creation nested;
    discovery_window window{[&] {
        if (!active) {
            return;
        }
        during_wait = dinput::native_control_reentrant();
        root.reset();
        nested = create(*borrowed_root, instance);
        observe(fixture);
        publish(fixture);
    }};
    std::promise<BOOL> queued;
    auto queued_result = queued.get_future();
    std::latch sender_exit{1};
    const std::jthread sender{[&] {
        const auto registered = wait_for_discovery(fixture);
        queued.set_value(
            registered ? SendNotifyMessageW(window.get(), discovery_window::notification, 0, 0)
                       : FALSE);
        sender_exit.wait();
    }};

    struct release_sender final {
        explicit release_sender(std::latch& value) noexcept : exit(value) {}

        release_sender(const release_sender&) = delete;
        release_sender& operator=(const release_sender&) = delete;
        release_sender(release_sender&&) = delete;
        release_sender& operator=(release_sender&&) = delete;
        std::latch& exit;

        ~release_sender() { exit.count_down(); }
    };

    const release_sender cleanup{sender_exit};

    active = true;
    const auto result = create(*borrowed_root, instance);
    active = false;
    REQUIRE(queued_result.wait_for(2s) == std::future_status::ready);
    REQUIRE(queued_result.get() != FALSE);
    CHECK_FALSE(window.failed());
    CHECK(during_wait);
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 6U);
    CHECK(nested.status == DI_OK);
    CHECK(axes(nested.device) == 0U);
    CHECK_FALSE(root);
}

TEST_CASE("dinput.discovery.shutdown_releases_registered_creation_without_session_ports",
    "[integration][dinput][input][discovery][lifecycle][concurrency]") {
    environment fixture;
    const auto instance = guid(708U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();
    auto pending = std::async(std::launch::async, [&] { return create(*root, instance); });
    const close_discovery cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    fixture.shutdown();
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    const auto result = pending.get();
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 0U);
}

TEST_CASE("dinput.discovery.late_connection_receives_new_budget_after_previous_timeout",
    "[integration][dinput][input][discovery][lifecycle][concurrency]") {
    environment fixture;
    set_budget(fixture, 250ms);
    const auto instance = guid(709U);
    fixture.add_system_gamepad(instance, native_startup_path);
    const auto root = fixture.create_root();
    const auto started = std::chrono::steady_clock::now();
    const auto unsupported = create(*root, instance);
    REQUIRE(unsupported.status == DI_OK);
    REQUIRE(axes(unsupported.device) == 0U);
    CHECK(std::chrono::steady_clock::now() - started >= 250ms);

    observe(fixture);
    auto pending = std::async(std::launch::async, [&] { return create(*root, instance); });
    const close_discovery cleanup{fixture.context.access};
    REQUIRE(wait_for_discovery(fixture));
    publish(fixture);
    const auto result = pending.get();
    REQUIRE(result.status == DI_OK);
    CHECK(axes(result.device) == 6U);
    CHECK(axes(unsupported.device) == 0U);
}

} // namespace inputnext::dinput_catalogue_test
