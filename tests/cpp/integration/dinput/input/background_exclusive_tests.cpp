#include "catalogue_test_support.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "dinput/com/device.hpp"
#include "dinput/exclusive/ownership.hpp"
#include "dinput/native_types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <inputnext/focus.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view device_path{R"(\\?\HID#VID_054C&PID_0CE6#BackgroundDevice)"};
constexpr DWORD background_exclusive = DISCL_BACKGROUND | DISCL_EXCLUSIVE;

class background_fixture final {
public:
    background_fixture() {
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
        if (wcscpy_s(path.wszPath, wide_path.c_str()) != 0) {
            throw std::runtime_error{"background native path is too long"};
        }
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
            throw std::runtime_error{"background device creation failed"};
        }
        device = com_owner<IDirectInputDevice8W>{static_cast<IDirectInputDevice8W*>(raw)};
        configure_compact_device(*device, window.get(), background_exclusive);
    }

    ~background_fixture() {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(native_operation::count);
            ++index) {
            catalogue.system->controls->release_block(static_cast<native_operation>(index));
        }
        device.reset();
        catalogue.context.access->close_admission();
        catalogue.context.access->drain();
    }

    background_fixture(const background_fixture&) = delete;
    background_fixture& operator=(const background_fixture&) = delete;
    background_fixture(background_fixture&&) = delete;
    background_fixture& operator=(background_fixture&&) = delete;

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
        : script_{std::move(script)},
          operation_{operation} {}

    ~unblock_native() { script_->release_block(operation_); }

    unblock_native(const unblock_native&) = delete;
    unblock_native& operator=(const unblock_native&) = delete;
    unblock_native(unblock_native&&) = delete;
    unblock_native& operator=(unblock_native&&) = delete;

private:
    const std::shared_ptr<native_control_script> script_;
    const native_operation operation_;
};

[[nodiscard]] std::optional<native_observation>
observed_operation(const native_control_script& script, const native_operation operation) {
    const auto observations = script.observations();
    const auto found = std::ranges::find_if(observations, [operation](const auto& observation) {
        return observation.operation == operation && observation.selected_device;
    });
    if (found == observations.end()) {
        return std::nullopt;
    }
    return *found;
}

void publish_expected_sample(background_fixture& fixture) {
    const auto expected = sample_state();
    publish_gamepad(*fixture.catalogue.owners, 0U, expected);
    if (!wait_for_sample(*fixture.catalogue.owners->core, fixture.target, expected)) {
        throw std::runtime_error{"normalized input sample was not published"};
    }
}

} // namespace

TEST_CASE("dinput.background_exclusive_uses_native_flags_and_ignores_foreground_changes",
    "[integration][dinput][exclusive][background]") {
    background_fixture fixture;
    publish_expected_sample(fixture);
    REQUIRE(fixture.device->Acquire() == DI_OK);

    const auto controls = fixture.catalogue.system->controls;
    const auto native_cooperation =
        observed_operation(*controls, native_operation::set_cooperative_level);
    if (!native_cooperation.has_value()) {
        FAIL("native background cooperative level was not observed");
        return;
    }
    const auto& cooperation = native_cooperation.value();
    CHECK(cooperation.flags == background_exclusive);
    CHECK(cooperation.window == fixture.window.get());
    CHECK(cooperation.single_threaded_apartment);

    const auto native_format = observed_operation(*controls, native_operation::set_data_format);
    if (!native_format.has_value()) {
        FAIL("native joystick format was not observed");
        return;
    }
    const auto* const format = native_format.value().format;
    REQUIRE(format != nullptr);
    CHECK(format->dwDataSize == data_size<DIJOYSTATE>());
    CHECK(controls->count(native_operation::get_device_state) >= 1U);

    compact_state state{};
    REQUIRE(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(state.axes[0U] == 40959L);
    CHECK(state.axes[1U] == 40959L);
    CHECK(state.axes[2U] == 16384L);
    CHECK(state.axes[3U] == 24575L);
    CHECK(state.axes[4U] == 24575L);
    CHECK(state.axes[5U] == 49151L);
    CHECK(state.buttons[0U] == 0x80U);
    CHECK(state.buttons[1U] == 0x80U);
    CHECK(state.pov == 4500U);
    CHECK(fixture.device->Poll() == DI_NOEFFECT);

    const hidden_window other_window;
    fixture.focus.set_foreground(false, other_window.id(), 3U);
    REQUIRE(fixture.focus.wait_for_evidence(focus::evidence::background));
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(fixture.device->Poll() == DI_NOEFFECT);

    fixture.focus.set_window(fixture.window.id(), 4U);
    REQUIRE(fixture.focus.wait_for_window(fixture.window.id(), 4U));
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.background_native_notacquired_revokes_buffered_input_and_requires_unacquire",
    "[integration][dinput][exclusive][background][buffered][concurrency]") {
    background_fixture fixture;
    publish_expected_sample(fixture);
    prepare_buffered_history(*fixture.catalogue.owners);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);
    const auto close_event = [](HANDLE handle) {
        static_cast<void>(CloseHandle(handle));
    };
    const std::unique_ptr<void, decltype(close_event)> event{
        CreateEventW(nullptr, FALSE, FALSE, nullptr),
        close_event};
    REQUIRE(event != nullptr);
    REQUIRE(fixture.device->SetEventNotification(event.get()) == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);

    const auto history = fixture.catalogue.owners->scripts[0U];
    history->block_next();
    core_test_support::history_call_release_guard release_history{history};
    REQUIRE(history->wait_until_blocked(2s));
    REQUIRE(ResetEvent(event.get()) != FALSE);

    const auto controls = fixture.catalogue.system->controls;
    controls->block_next(native_operation::unacquire, true);
    controls->set_result(native_operation::get_device_state, DIERR_NOTACQUIRED);
    const unblock_native release_native{controls, native_operation::unacquire};
    REQUIRE(controls->wait_for_entry(native_operation::unacquire, 1U, 5s));
    fixture.catalogue.context.access->refresh_eligibility();
    CHECK(WaitForSingleObject(event.get(), 0U) == WAIT_OBJECT_0);
    fixture.catalogue.context.access->refresh_eligibility();
    CHECK(WaitForSingleObject(event.get(), 0U) == WAIT_TIMEOUT);

    struct read_result final {
        HRESULT state{};
        HRESULT data{};
        HRESULT poll{};
        DWORD data_count{};
        bool state_unchanged{};
    };

    std::future<read_result> reads;
    std::future<HRESULT> retrying_acquire;
    reads = std::async(std::launch::async, [&fixture] {
        auto state = sentinel_state(0xA5U);
        const auto sentinel = state;
        const auto state_status =
            fixture.device->GetDeviceState(data_size<compact_state>(), &state);
        std::array<DIDEVICEOBJECTDATA, 8U> data{};
        auto count = static_cast<DWORD>(data.size());
        const auto data_status =
            fixture.device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), data.data(), &count, 0U);
        const auto poll_status = fixture.device->Poll();
        return read_result{.state = state_status,
            .data = data_status,
            .poll = poll_status,
            .data_count = count,
            .state_unchanged = state == sentinel};
    });
    retrying_acquire =
        std::async(std::launch::async, [&fixture] { return fixture.device->Acquire(); });
    const auto reads_ready = reads.wait_for(2s) == std::future_status::ready;
    const auto retry_ready = retrying_acquire.wait_for(2s) == std::future_status::ready;
    const auto acquire_calls_before_release = controls->count(native_operation::acquire);
    const auto probe_calls_while_blocked = controls->count(native_operation::get_device_state);

    release_history.release();
    controls->release_block(native_operation::unacquire);
    const auto read = reads.get();
    const auto acquire_status = retrying_acquire.get();

    CHECK(reads_ready);
    CHECK(retry_ready);
    CHECK(read.state == DIERR_NOTACQUIRED);
    CHECK(read.data == DIERR_NOTACQUIRED);
    CHECK(read.poll == DIERR_NOTACQUIRED);
    CHECK(read.data_count == 0U);
    CHECK(read.state_unchanged);
    CHECK(probe_calls_while_blocked >= 2U);
    CHECK(acquire_status == DIERR_NOTACQUIRED);
    CHECK(controls->count(native_operation::acquire) == acquire_calls_before_release);

    CHECK(fixture.device->Unacquire() == DI_OK);
    controls->set_result(native_operation::get_device_state, DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(controls->count(native_operation::acquire) == acquire_calls_before_release + 1U);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.background_initial_notacquired_checkpoint_requires_explicit_unacquire",
    "[integration][dinput][exclusive][background][lifecycle]") {
    background_fixture fixture;
    const auto controls = fixture.catalogue.system->controls;
    controls->set_result(native_operation::get_device_state, DIERR_NOTACQUIRED);

    CHECK(fixture.device->Acquire() == DIERR_NOTACQUIRED);
    const auto acquire_calls = controls->count(native_operation::acquire);
    CHECK(acquire_calls == 1U);
    CHECK(fixture.device->Acquire() == DIERR_NOTACQUIRED);
    CHECK(controls->count(native_operation::acquire) == acquire_calls);

    CHECK(fixture.device->Unacquire() == DI_OK);
    controls->set_result(native_operation::get_device_state, DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);
    CHECK(controls->count(native_operation::acquire) == acquire_calls + 1U);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.background_pending_buffered_acquire_expires_during_a_blocked_checkpoint",
    "[integration][dinput][exclusive][background][buffered][expiry][concurrency]") {
    background_fixture fixture;
    prepare_buffered_history(*fixture.catalogue.owners);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);

    const auto history = fixture.catalogue.owners->scripts[0U];
    history->block_next();
    std::future<HRESULT> acquiring;
    core_test_support::history_call_release_guard release_history{history};
    const auto controls = fixture.catalogue.system->controls;
    acquiring = std::async(std::launch::async, [&fixture] { return fixture.device->Acquire(); });
    REQUIRE(history->wait_until_blocked(5s));

    const auto initial_probe_count = controls->count(native_operation::get_device_state);
    controls->block_next(native_operation::get_device_state, true);
    const unblock_native release_native{controls, native_operation::get_device_state};
    REQUIRE(controls->wait_for_entry(native_operation::get_device_state, 1U, 5s));

    const auto expired = acquiring.wait_for(5s) == std::future_status::ready;
    release_history.release();
    controls->release_block(native_operation::get_device_state);
    const auto acquisition = acquiring.get();

    CHECK(expired);
    CHECK(controls->count(native_operation::get_device_state) > initial_probe_count);
    CHECK(acquisition == DIERR_INPUTLOST);
    CHECK(controls->count(native_operation::acquire) == 1U);
}

TEST_CASE("dinput.background_target_disconnect_is_visible_during_a_blocked_native_probe",
    "[integration][dinput][exclusive][background][reconnect]") {
    background_fixture fixture;
    publish_expected_sample(fixture);
    REQUIRE(fixture.device->Acquire() == DI_OK);

    const auto controls = fixture.catalogue.system->controls;
    controls->block_next(native_operation::get_device_state, true);
    const unblock_native release_native{controls, native_operation::get_device_state};
    REQUIRE(controls->wait_for_entry(native_operation::get_device_state, 1U, 5s));
    REQUIRE(fixture.catalogue.disconnect(0U, fixture.target));

    auto state = sentinel_state(0x63U);
    const auto sentinel = state;
    CHECK(fixture.device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);
    controls->release_block(native_operation::get_device_state);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.background_cached_reads_complete_while_native_health_probe_is_blocked",
    "[integration][dinput][exclusive][background][concurrency]") {
    background_fixture fixture;
    const auto controls = fixture.catalogue.system->controls;
    controls->block_next(native_operation::get_device_state, true);
    std::future<HRESULT> acquiring;
    std::future<std::array<HRESULT, 3U>> reads;
    const unblock_native release_native{controls, native_operation::get_device_state};
    acquiring = std::async(std::launch::async, [&fixture] { return fixture.device->Acquire(); });
    REQUIRE(controls->wait_for_entry(native_operation::get_device_state, 1U, 5s));
    const auto native_reads_at_barrier = controls->count(native_operation::get_device_state);

    reads = std::async(std::launch::async, [&fixture] {
        auto state = sentinel_state(0x9cU);
        const auto sentinel = state;
        const auto state_status =
            fixture.device->GetDeviceState(data_size<compact_state>(), &state);
        return std::array{state_status, fixture.device->Poll(), state == sentinel ? S_OK : E_FAIL};
    });
    const auto reads_ready = reads.wait_for(5s) == std::future_status::ready;
    const auto native_reads_while_blocked = controls->count(native_operation::get_device_state);
    controls->release_block(native_operation::get_device_state);
    const auto statuses = reads.get();
    const auto acquire_ready = acquiring.wait_for(5s) == std::future_status::ready;
    const auto acquire_status = acquiring.get();

    CHECK(reads_ready);
    CHECK(acquire_ready);
    CHECK(native_reads_while_blocked == native_reads_at_barrier);
    CHECK(statuses[0U] == DIERR_NOTACQUIRED);
    CHECK(statuses[1U] == DIERR_NOTACQUIRED);
    CHECK(statuses[2U] == S_OK);
    CHECK(acquire_status == DI_OK);
    CHECK(fixture.device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.background_acquired_reads_use_cache_during_a_blocked_refresh",
    "[integration][dinput][exclusive][background][buffered][concurrency]") {
    background_fixture fixture;
    publish_expected_sample(fixture);
    prepare_buffered_history(*fixture.catalogue.owners);
    REQUIRE(set_buffer_size(*fixture.device, 8U) == DI_OK);
    REQUIRE(fixture.device->Acquire() == DI_OK);

    const auto controls = fixture.catalogue.system->controls;
    controls->block_next(native_operation::get_device_state, true);
    const unblock_native release_native{controls, native_operation::get_device_state};
    REQUIRE(controls->wait_for_entry(native_operation::get_device_state, 1U, 2s));
    const auto native_reads = controls->count(native_operation::get_device_state);

    auto reads = std::async(std::launch::async, [&fixture] {
        compact_state state{};
        const auto latest = fixture.device->GetDeviceState(data_size<compact_state>(), &state);
        DIDEVICEOBJECTDATA event{};
        DWORD count = 1U;
        const auto buffered = fixture.device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
            &event,
            &count,
            DIGDD_PEEK);
        return std::array{latest, buffered, fixture.device->Poll(), state.axes[0U]};
    });
    const auto status = reads.wait_for(2s);
    const auto native_reads_after = controls->count(native_operation::get_device_state);
    controls->release_block(native_operation::get_device_state);
    const auto results = reads.get();

    CHECK(status == std::future_status::ready);
    CHECK(native_reads_after == native_reads);
    CHECK(results[0U] == DI_OK);
    CHECK(SUCCEEDED(results[1U]));
    CHECK(results[2U] == DI_NOEFFECT);
    CHECK(results[3U] == 40959L);
    CHECK(fixture.device->Unacquire() == DI_OK);
}
