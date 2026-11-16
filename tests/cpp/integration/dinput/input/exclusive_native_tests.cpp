#include "catalogue_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/exclusive/lease.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/native_types.hpp"
#include "dinput/system_input.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view device_path{R"(\\?\HID#VID_054C&PID_0CE6#NativeActor)"};
constexpr std::wstring_view native_device_path{LR"(\\?\HID#VID_054C&PID_0CE6#NativeActor)"};

using lease_result = std::expected<std::shared_ptr<dinput::exclusive_lease>, HRESULT>;

class actor_clock_gate final {
public:
    using time_point = std::chrono::steady_clock::time_point;

    [[nodiscard]] time_point now() const noexcept {
        std::unique_lock lock{gate_};
        if (armed_ && actor_ == std::this_thread::get_id()) {
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
            armed_ = false;
        }
        return clock_.now();
    }

    [[nodiscard]] static time_point read(const void* context) noexcept {
        return static_cast<const actor_clock_gate*>(context)->now();
    }

    void arm(const std::thread::id actor) noexcept {
        std::scoped_lock lock{gate_};
        actor_ = actor;
        armed_ = true;
        blocked_ = false;
        released_ = false;
    }

    [[nodiscard]] bool wait_until_blocked(const std::chrono::milliseconds timeout = 2s) const {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this] { return blocked_; });
    }

    void release() const noexcept {
        std::scoped_lock lock{gate_};
        released_ = true;
        changed_.notify_all();
    }

private:
    mutable std::mutex gate_;
    mutable std::condition_variable changed_;
    mutable std::thread::id actor_{};
    mutable bool armed_{};
    mutable bool blocked_{};
    mutable bool released_{};
    test::focus::manual_clock clock_;
};

class native_service_fixture final {
public:
    explicit native_service_fixture(const focus_window_fixture::clock_callback now = nullptr,
        const void* clock_context = nullptr)
        : focus{now, clock_context} {
        focus.set_window(window.id(), 2U);
        if (!focus.wait_for_window(window.id(), 2U)) {
            throw std::runtime_error{"foreground observation unavailable"};
        }
        catalogue.context.access = make_access(*catalogue.owners, focus.observer());
        catalogue.add_system_gamepad(native_guid, std::wstring{native_device_path});
        catalogue.system->controls = std::make_shared<native_control_script>();
        target = catalogue.connect(0U, std::string{device_path});
        if (!catalogue.wait_for_catalogue(1U)) {
            throw std::runtime_error{"input catalogue unavailable"};
        }
        provider = std::make_shared<native_factory_context>();
        provider->system = catalogue.system;
        factory = dinput::native_input_factory{.create = &create_native_system,
            .context = provider.get(),
            .instance = GetModuleHandleW(nullptr),
            .lifetime = provider};
    }

    ~native_service_fixture() {
        release_all_native_blocks();
        catalogue.context.access->close_admission();
        catalogue.context.access->drain();
    }

    native_service_fixture(const native_service_fixture&) = delete;
    native_service_fixture& operator=(const native_service_fixture&) = delete;
    native_service_fixture(native_service_fixture&&) = delete;
    native_service_fixture& operator=(native_service_fixture&&) = delete;

    [[nodiscard]] dinput::exclusive_request request(const GUID requested_guid = native_guid,
        const std::wstring_view requested_path = native_device_path) {
        const auto presentation = focus.observer().lock_presentation();
        const auto epoch = presentation.current_foreground_window_epoch(window.id());
        if (!epoch.has_value()) {
            throw std::runtime_error{"foreground window epoch unavailable"};
        }

        DIPROPGUIDANDPATH path{};
        path.diph.dwSize = static_cast<DWORD>(sizeof(path));
        path.diph.dwHeaderSize = static_cast<DWORD>(sizeof(DIPROPHEADER));
        path.diph.dwHow = DIPH_DEVICE;
        path.guidClass = GUID_Joystick;
        const std::wstring path_value{requested_path};
        if (wcscpy_s(path.wszPath, path_value.c_str()) != 0) {
            throw std::runtime_error{"native path exceeds DirectInput capacity"};
        }
        return dinput::exclusive_request{.target = target,
            .native_instance = requested_guid,
            .native_path = path,
            .window = *epoch,
            .factory = factory};
    }

    [[nodiscard]] lease_result reserve(dinput::exclusive_request value) const {
        const auto call = catalogue.context.access->enter();
        if (!call) {
            return std::unexpected{DIERR_INPUTLOST};
        }
        return call.exclusives().reserve(std::move(value));
    }

    void release_all_native_blocks() const noexcept {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(native_operation::count);
            ++index) {
            catalogue.system->controls->release_block(static_cast<native_operation>(index));
        }
        catalogue.system->controls->clear_send_message();
    }

    [[nodiscard]] std::shared_ptr<native_control_script> controls() const noexcept {
        return catalogue.system->controls;
    }

    hidden_window window;
    focus_window_fixture focus;
    environment catalogue;
    connection_target target{};
    std::shared_ptr<native_factory_context> provider;
    dinput::native_input_factory factory;

private:
    inline static const GUID native_guid = guid(1U);
};

class release_actor_clock_on_exit final {
public:
    explicit release_actor_clock_on_exit(const actor_clock_gate& clock) noexcept : clock_(clock) {}

    ~release_actor_clock_on_exit() { clock_.release(); }

    release_actor_clock_on_exit(const release_actor_clock_on_exit&) = delete;
    release_actor_clock_on_exit& operator=(const release_actor_clock_on_exit&) = delete;
    release_actor_clock_on_exit(release_actor_clock_on_exit&&) = delete;
    release_actor_clock_on_exit& operator=(release_actor_clock_on_exit&&) = delete;

private:
    const actor_clock_gate& clock_;
};

[[nodiscard]] std::vector<native_operation> selected_operations(
    const std::vector<native_observation>& observations) {
    std::vector<native_operation> result;
    for (const auto& observation : observations) {
        if (observation.selected_device) {
            result.push_back(observation.operation);
        }
    }
    return result;
}

[[nodiscard]] std::size_t selected_count(const native_control_script& controls,
    const native_operation operation) {
    const auto observations = controls.observations();
    return static_cast<std::size_t>(
        std::ranges::count_if(observations, [operation](const native_observation& observation) {
            return observation.operation == operation && observation.selected_device;
        }));
}

struct disposal_gate final {
    [[nodiscard]] bool wait_until_started(const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{gate};
        return changed.wait_for(lock, timeout, [this] { return started; });
    }

    void release() noexcept {
        std::scoped_lock lock{gate};
        released = true;
        changed.notify_all();
    }

    [[nodiscard]] bool finished() noexcept {
        std::scoped_lock lock{gate};
        return disposed;
    }

    std::mutex gate;
    std::condition_variable changed;
    bool started{};
    bool released{};
    bool disposed{};
};

class release_disposal_on_exit final {
public:
    explicit release_disposal_on_exit(std::shared_ptr<disposal_gate> signal) noexcept
        : signal_(std::move(signal)) {}

    ~release_disposal_on_exit() { signal_->release(); }

    release_disposal_on_exit(const release_disposal_on_exit&) = delete;
    release_disposal_on_exit& operator=(const release_disposal_on_exit&) = delete;
    release_disposal_on_exit(release_disposal_on_exit&&) = delete;
    release_disposal_on_exit& operator=(release_disposal_on_exit&&) = delete;

private:
    const std::shared_ptr<disposal_gate> signal_;
};

void dispose_after_release(native_factory_context* provider,
    const std::shared_ptr<disposal_gate>& signal) noexcept {
    {
        std::unique_lock lock{signal->gate};
        signal->started = true;
        signal->changed.notify_all();
        signal->changed.wait(lock, [&signal] { return signal->released; });
    }
    delete provider;
    {
        std::scoped_lock lock{signal->gate};
        signal->disposed = true;
        signal->changed.notify_all();
    }
}

} // namespace

TEST_CASE("dinput.exclusive_native_factory_accepts_only_safe_context_lifetimes",
    "[integration][dinput][exclusive][native][policy]") {
    const dinput::native_input_factory production{.create = &dinput::create_system_input,
        .context = nullptr,
        .instance = GetModuleHandleW(nullptr)};
    CHECK(production.supported());

    int marker{};
    const dinput::native_input_factory default_with_context{.create = &dinput::create_system_input,
        .context = &marker,
        .instance = GetModuleHandleW(nullptr)};
    CHECK(!default_with_context.supported());

    const dinput::native_input_factory custom_without_lifetime{.create = &create_native_system,
        .context = &marker,
        .instance = GetModuleHandleW(nullptr)};
    CHECK(!custom_without_lifetime.supported());

    const auto lifetime = std::make_shared<int>(1);
    const dinput::native_input_factory custom_with_lifetime{.create = &create_native_system,
        .context = lifetime.get(),
        .instance = GetModuleHandleW(nullptr),
        .lifetime = lifetime};
    CHECK(custom_with_lifetime.supported());
}

TEST_CASE("dinput.exclusive_native_reservation_is_inert_until_started_and_reaps_after_close",
    "[integration][dinput][exclusive][native][lifecycle]") {
    native_service_fixture fixture;
    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    CHECK(fixture.controls()->count(native_operation::factory_create) == 0U);
    CHECK(fixture.controls()->count(native_operation::enum_devices) == 0U);
    lease->cancel();
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(fixture.controls()->count(native_operation::factory_create) == 0U);
    CHECK(fixture.controls()->count(native_operation::release_root) == 0U);

    auto next = fixture.reserve(fixture.request(guid(2U), L"A distinct native path"));
    REQUIRE(next.has_value());
    (*next)->cancel();
    CHECK((*next)->wait_closed() == DI_OK);
}

TEST_CASE("dinput.exclusive_native_actor_uses_a_private_sta_and_exact_native_controls",
    "[integration][dinput][exclusive][native][actor]") {
    native_service_fixture fixture;
    const auto controls = fixture.controls();
    controls->send_message_on(native_operation::factory_create,
        native_message{.window = fixture.window.get(), .message = WM_NULL});

    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(lease->wait_prepared() == DI_OK);
    REQUIRE(lease->try_activate());
    CHECK(lease->live());
    lease->cancel();
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(!lease->live());

    const auto observations = controls->observations();
    const auto expected_selected = std::vector{native_operation::set_data_format,
        native_operation::set_cooperative_level,
        native_operation::acquire,
        native_operation::guid_and_path,
        native_operation::unacquire,
        native_operation::release_device};
    CHECK(selected_operations(observations) == expected_selected);
    CHECK(std::ranges::all_of(observations, [](const native_observation& observation) {
        return observation.single_threaded_apartment;
    }));
    CHECK(controls->count(native_operation::factory_create) == 1U);
    CHECK(controls->count(native_operation::enum_devices) == 2U);
    CHECK(controls->count(native_operation::release_root) == 1U);
    CHECK(controls->last_thread(native_operation::factory_create) != std::this_thread::get_id());
    CHECK(controls->last_thread(native_operation::factory_create) ==
          controls->last_thread(native_operation::release_root));
    CHECK(controls->last_thread(native_operation::set_data_format) ==
          controls->last_thread(native_operation::release_device));
    CHECK(selected_count(*controls, native_operation::unacquire) == 1U);

    const auto selected_format =
        std::ranges::find_if(observations, [](const native_observation& observation) {
            return observation.operation == native_operation::set_data_format &&
                   observation.selected_device;
        });
    REQUIRE(selected_format != observations.end());
    CHECK(selected_format->format == dinput::system_joystick_format());

    const auto selected_cooperative =
        std::ranges::find_if(observations, [](const native_observation& observation) {
            return observation.operation == native_operation::set_cooperative_level &&
                   observation.selected_device;
        });
    REQUIRE(selected_cooperative != observations.end());
    CHECK(selected_cooperative->window == fixture.window.get());
    CHECK(selected_cooperative->flags == (DISCL_FOREGROUND | DISCL_EXCLUSIVE));
}

TEST_CASE("dinput.exclusive_native_failures_preserve_hresult_and_release_owned_interfaces",
    "[integration][dinput][exclusive][native][failure]") {
    native_service_fixture fixture;
    HRESULT expected{};
    bool selected_device_opened{};
    bool acquire_attempted{};
    bool root_created{};

    SECTION("factory failure") {
        expected = E_ACCESSDENIED;
        fixture.controls()->set_result(native_operation::factory_create, expected);
    }
    SECTION("strict native enumeration failure") {
        expected = E_ABORT;
        fixture.controls()->set_result(native_operation::enum_devices, expected);
        root_created = true;
    }
    SECTION("native competitor CreateDevice failure") {
        expected = E_ACCESSDENIED;
        std::scoped_lock lock{fixture.catalogue.system->gate};
        fixture.catalogue.system->devices.back().create_device_result = expected;
        root_created = true;
    }
    SECTION("native GUIDANDPATH failure") {
        expected = E_FAIL;
        std::scoped_lock lock{fixture.catalogue.system->gate};
        fixture.catalogue.system->devices.back().guid_path_result = expected;
        root_created = true;
    }
    SECTION("SetDataFormat failure") {
        expected = E_ACCESSDENIED;
        fixture.controls()->set_result(native_operation::set_data_format, expected);
        root_created = true;
        selected_device_opened = true;
    }
    SECTION("SetCooperativeLevel failure") {
        expected = E_FAIL;
        fixture.controls()->set_result(native_operation::set_cooperative_level, expected);
        root_created = true;
        selected_device_opened = true;
    }
    SECTION("Acquire failure") {
        expected = E_ACCESSDENIED;
        fixture.controls()->set_result(native_operation::acquire, expected);
        root_created = true;
        selected_device_opened = true;
        acquire_attempted = true;
    }

    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    CHECK(lease->wait_prepared() == expected);
    CHECK(!lease->try_activate());
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(fixture.controls()->count(native_operation::release_root) == (root_created ? 1U : 0U));
    CHECK(selected_count(*fixture.controls(), native_operation::unacquire) ==
          (acquire_attempted ? 1U : 0U));
    CHECK(selected_count(*fixture.controls(), native_operation::release_device) ==
          (selected_device_opened ? 1U : 0U));
}

TEST_CASE("dinput.exclusive_native_cancellation_cleans_up_a_late_native_acquire",
    "[integration][dinput][exclusive][native][cancellation]") {
    native_service_fixture fixture;
    const auto controls = fixture.controls();
    controls->block_next(native_operation::acquire, true);

    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::acquire));
    lease->cancel();
    CHECK(!lease->try_activate());
    controls->release_block(native_operation::acquire);
    CHECK(lease->wait_prepared() == DIERR_INPUTLOST);
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(selected_count(*controls, native_operation::acquire) == 1U);
    CHECK(selected_count(*controls, native_operation::unacquire) == 1U);
    CHECK(selected_count(*controls, native_operation::release_device) == 1U);
    CHECK(controls->count(native_operation::release_root) == 1U);
}

TEST_CASE("dinput.exclusive_native_cancellation_during_focus_validation_prevents_acquire",
    "[integration][dinput][exclusive][native][cancellation]") {
    actor_clock_gate clock;
    native_service_fixture fixture{&actor_clock_gate::read, &clock};
    const auto controls = fixture.controls();
    controls->block_next(native_operation::set_cooperative_level, true);

    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::set_cooperative_level));

    clock.arm(controls->last_thread(native_operation::set_cooperative_level));
    const release_actor_clock_on_exit release_clock{clock};
    controls->release_block(native_operation::set_cooperative_level);
    REQUIRE(clock.wait_until_blocked());

    lease->cancel();
    clock.release();
    CHECK(lease->wait_prepared() == DIERR_INPUTLOST);
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(selected_count(*controls, native_operation::acquire) == 0U);
    CHECK(selected_count(*controls, native_operation::unacquire) == 0U);
    CHECK(selected_count(*controls, native_operation::release_device) == 1U);
}

TEST_CASE("dinput.exclusive_native_cancellation_after_enum_stops_strict_native_sweep",
    "[integration][dinput][exclusive][native][cancellation]") {
    native_service_fixture fixture;
    const auto controls = fixture.controls();
    controls->block_next(native_operation::acquire, true);

    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::acquire));
    controls->block_next(native_operation::enum_devices);
    controls->release_block(native_operation::acquire);
    REQUIRE(controls->wait_for_entry(native_operation::enum_devices));
    const auto create_device_calls = controls->count(native_operation::create_device);

    lease->cancel();
    controls->release_block(native_operation::enum_devices);
    CHECK(lease->wait_prepared() == DIERR_INPUTLOST);
    CHECK(lease->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::create_device) == create_device_calls);
    CHECK(selected_count(*controls, native_operation::unacquire) == 1U);
    CHECK(selected_count(*controls, native_operation::release_device) == 1U);
    CHECK(controls->count(native_operation::release_root) == 1U);
}

TEST_CASE("dinput.exclusive_native_rejects_opened_path_drift_and_reports_final_unacquire_failure",
    "[integration][dinput][exclusive][native][failure]") {
    native_service_fixture fixture;
    SECTION("opened path drifts after native Acquire") {
        fixture.controls()->set_opened_path_after_acquire(L"A different opened path");
        auto reserved = fixture.reserve(fixture.request());
        REQUIRE(reserved.has_value());
        const auto& lease = *reserved;
        REQUIRE(lease->start() == DI_OK);
        CHECK(lease->wait_prepared() == DIERR_INPUTLOST);
        CHECK(lease->wait_closed() == DI_OK);
        CHECK(selected_count(*fixture.controls(), native_operation::acquire) == 1U);
        CHECK(selected_count(*fixture.controls(), native_operation::unacquire) == 1U);
        CHECK(selected_count(*fixture.controls(), native_operation::release_device) == 1U);
    }
    SECTION("final native Unacquire fails") {
        fixture.controls()->set_result(native_operation::unacquire, E_ACCESSDENIED);
        auto reserved = fixture.reserve(fixture.request());
        REQUIRE(reserved.has_value());
        const auto& lease = *reserved;
        REQUIRE(lease->start() == DI_OK);
        REQUIRE(lease->wait_prepared() == DI_OK);
        REQUIRE(lease->try_activate());
        lease->cancel();
        CHECK(lease->wait_closed() == E_ACCESSDENIED);
        CHECK(selected_count(*fixture.controls(), native_operation::unacquire) == 1U);
        CHECK(selected_count(*fixture.controls(), native_operation::release_device) == 1U);
        CHECK(fixture.controls()->count(native_operation::release_root) == 1U);
    }
}

TEST_CASE("dinput.exclusive_native_conflicts_cover_guid_and_case_insensitive_path",
    "[integration][dinput][exclusive][native][resources]") {
    native_service_fixture fixture;
    auto first = fixture.reserve(fixture.request());
    REQUIRE(first.has_value());

    auto guid_conflict = fixture.reserve(fixture.request(guid(1U), L"Different path"));
    REQUIRE(!guid_conflict.has_value());
    CHECK(guid_conflict.error() == HRESULT_FROM_WIN32(ERROR_BUSY));

    auto path_conflict =
        fixture.reserve(fixture.request(guid(2U), L"\\\\?\\HID#VID_054C&PID_0CE6#nativeactor"));
    REQUIRE(!path_conflict.has_value());
    CHECK(path_conflict.error() == HRESULT_FROM_WIN32(ERROR_BUSY));

    (*first)->cancel();
    CHECK((*first)->wait_closed() == DI_OK);
    auto replacement = fixture.reserve(fixture.request(guid(2U), L"Different path"));
    REQUIRE(replacement.has_value());
    (*replacement)->cancel();
    CHECK((*replacement)->wait_closed() == DI_OK);
}

TEST_CASE("dinput.exclusive_native_capacity_counts_reserved_and_closing_actors",
    "[integration][dinput][exclusive][native][resources]") {
    native_service_fixture fixture;
    std::array<std::shared_ptr<dinput::exclusive_lease>, 16U> leases{};
    for (std::size_t index = 0U; index < leases.size(); ++index) {
        auto reserved =
            fixture.reserve(fixture.request(guid(static_cast<std::uint32_t>(index + 1U)),
                L"Native actor slot " + std::to_wstring(index)));
        REQUIRE(reserved.has_value());
        leases[index] = *reserved;
    }

    auto full = fixture.reserve(fixture.request(guid(100U), L"Native actor overflow"));
    REQUIRE(!full.has_value());
    CHECK(full.error() == DIERR_OUTOFMEMORY);
    CHECK(fixture.controls()->count(native_operation::factory_create) == 0U);

    for (const auto& lease : leases) {
        lease->cancel();
    }
    for (const auto& lease : leases) {
        CHECK(lease->wait_closed() == DI_OK);
    }
}

TEST_CASE("dinput.exclusive_native_provider_disposal_keeps_its_slot_conflicted",
    "[integration][dinput][exclusive][native][resources][concurrency]") {
    native_service_fixture fixture;
    const auto disposal = std::make_shared<disposal_gate>();
    auto* const raw_provider = new native_factory_context{.system = fixture.catalogue.system};
    std::shared_ptr<native_factory_context> provider{raw_provider,
        [disposal](native_factory_context* value) { dispose_after_release(value, disposal); }};

    auto request = fixture.request();
    request.factory.context = provider.get();
    request.factory.lifetime = provider;
    provider.reset();
    auto first = fixture.reserve(std::move(request));
    REQUIRE(first.has_value());
    const auto& closing_lease = *first;
    closing_lease->cancel();
    CHECK(closing_lease->wait_closed() == DI_OK);

    auto reap = std::async(std::launch::async, [&fixture] {
        return fixture.reserve(fixture.request(guid(2U), L"Provider disposal successor"));
    });
    const release_disposal_on_exit release_disposal{disposal};
    REQUIRE(disposal->wait_until_started());

    auto conflicting = fixture.reserve(fixture.request(guid(1U), L"Different path"));
    REQUIRE(!conflicting.has_value());
    CHECK(conflicting.error() == HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(!disposal->finished());
    CHECK(reap.wait_for(0ms) == std::future_status::timeout);

    disposal->release();
    auto successor = reap.get();
    REQUIRE(successor.has_value());
    CHECK(disposal->finished());
    (*successor)->cancel();
    CHECK((*successor)->wait_closed() == DI_OK);
}

TEST_CASE("dinput.exclusive_native_drain_waits_for_a_blocked_native_call_and_final_release",
    "[integration][dinput][exclusive][native][lifecycle][concurrency]") {
    native_service_fixture fixture;
    const auto controls = fixture.controls();
    controls->block_next(native_operation::acquire, true);
    auto reserved = fixture.reserve(fixture.request());
    REQUIRE(reserved.has_value());
    const auto& lease = *reserved;
    REQUIRE(lease->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::acquire));

    auto drained =
        std::async(std::launch::async, [&fixture] { fixture.catalogue.context.access->drain(); });
    REQUIRE(WaitForSingleObject(lease->cancellation_event(), 2'000U) == WAIT_OBJECT_0);
    CHECK(drained.wait_for(10ms) == std::future_status::timeout);

    controls->release_block(native_operation::acquire);
    CHECK(drained.wait_for(2s) == std::future_status::ready);
    drained.get();
    CHECK(lease->closed());
    CHECK(lease->wait_prepared() == DIERR_INPUTLOST);
    CHECK(selected_count(*controls, native_operation::unacquire) == 1U);
    CHECK(selected_count(*controls, native_operation::release_device) == 1U);
    CHECK(controls->count(native_operation::release_root) == 1U);
}
