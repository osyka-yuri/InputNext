#include "support/devices/read_pin_test_access.hpp"
#include "support/xinput/service_test_access.hpp"
#include "support/xinput/test_ports.hpp"

#include "devices/connection_record.hpp"
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/latest_value.hpp>
#include <inputnext/source_lifetime.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::xinput_test_support;
using namespace std::chrono_literals;

using prepared_read = xinput::detail::service_test_access::prepared_read;

[[nodiscard]] XINPUT_STATE sentinel_state() noexcept {
    XINPUT_STATE state{.dwPacketNumber = 0xC0FFEEU, .Gamepad = {}};
    state.Gamepad.wButtons = 0xA55AU;
    return state;
}

struct commit_observation final {
    bool completed_under_gate{};
    DWORD status{};
    XINPUT_STATE state{sentinel_state()};
};

[[nodiscard]] commit_observation commit_with_connection_locked(xinput::service& service,
    const prepared_read& prepared,
    bool close_sample) {
    commit_observation observed{};
    auto& record = detail::read_pin_test_access::record(prepared.connection_pin);
    std::packaged_task<DWORD()> task{[&] {
        return xinput::detail::service_test_access::commit(service, prepared, &observed.state);
    }};
    auto completed = task.get_future();
    std::jthread worker;
    {
        std::unique_lock gate{record.gate};
        if (close_sample) {
            if (!record.latest.has_value()) {
                throw std::logic_error{"A pinned sample is required"};
            }
            record.latest.value().close();
        }
        worker = std::jthread{std::move(task)};
        observed.completed_under_gate = completed.wait_for(2s) == std::future_status::ready;
    }
    worker.join();
    observed.status = completed.get();
    return observed;
}

TEST_CASE("GetState confirms a retained pin without waiting for the connection gate",
    "[component][xinput][concurrency]") {
    const bool enabled = GENERATE(true, false);
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection =
        devices.add(gamepad_state{.buttons = static_cast<button_mask>(gamepad_button::south)});
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());
    service.enable(enabled ? TRUE : FALSE);
    prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant->user_index,
                false,
                prepared) == ERROR_SUCCESS);

    const auto observed = commit_with_connection_locked(service, prepared, false);
    CHECK(observed.completed_under_gate);
    CHECK(observed.status == ERROR_SUCCESS);
    CHECK(observed.state.dwPacketNumber == (enabled ? 1U : 0U));
    CHECK(observed.state.Gamepad.wButtons == (enabled ? XINPUT_GAMEPAD_A : 0U));
}

TEST_CASE("GetState rejects sample closure before retirement phase publication",
    "[component][xinput][concurrency]") {
    const bool enabled = GENERATE(true, false);
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());
    service.enable(enabled ? TRUE : FALSE);
    prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant->user_index,
                false,
                prepared) == ERROR_SUCCESS);

    const auto observed = commit_with_connection_locked(service, prepared, true);
    CHECK(observed.completed_under_gate);
    CHECK(detail::read_pin_test_access::record(prepared.connection_pin)
              .phase.load(std::memory_order_seq_cst) == detail::connection_phase::live);
    CHECK(observed.status == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(observed.state.dwPacketNumber == sentinel_state().dwPacketNumber);
    CHECK(observed.state.Gamepad.wButtons == sentinel_state().Gamepad.wButtons);
}

TEST_CASE("GetState rejects retired targets while their XInput assignment remains",
    "[component][xinput][lifetime]") {
    const bool enabled = GENERATE(true, false);
    const bool replace = GENERATE(false, true);
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());
    service.enable(enabled ? TRUE : FALSE);
    prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant->user_index,
                false,
                prepared) == ERROR_SUCCESS);

    if (replace) {
        std::unique_ptr<source_lifetime> source = std::make_unique<test_source_lifetime>();
        REQUIRE(devices.registry().replace(connection.connection.target, source).has_value());
    } else {
        REQUIRE(devices.registry().retire(connection.connection.target).has_value());
    }
    CHECK(service.positions()[occupant->user_index] == *occupant);
    CHECK(devices.registry().drain_retired() == 0U);
    auto state = sentinel_state();
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == sentinel_state().dwPacketNumber);
    CHECK(state.Gamepad.wButtons == sentinel_state().Gamepad.wButtons);
    prepared.connection_pin.release();
    CHECK(devices.registry().drain_retired() == 1U);
}

TEST_CASE("GetState releases its retained connection after shutdown invalidates confirmation",
    "[component][xinput][lifetime][concurrency]") {
    const bool enabled = GENERATE(true, false);
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());
    service.enable(enabled ? TRUE : FALSE);
    std::packaged_task<void()> task{[&] { devices.registry().shutdown(); }};
    auto completed = task.get_future();
    std::jthread worker;
    prepared_read prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare(service,
                occupant->user_index,
                false,
                prepared) == ERROR_SUCCESS);
    worker = std::jthread{std::move(task)};

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (prepared.connection_pin.validate_live().has_value() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(!prepared.connection_pin.validate_live().has_value());
    CHECK(completed.wait_for(0ms) == std::future_status::timeout);
    auto state = sentinel_state();
    CHECK(xinput::detail::service_test_access::commit(service, prepared, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == sentinel_state().dwPacketNumber);
    CHECK(state.Gamepad.wButtons == sentinel_state().Gamepad.wButtons);
    prepared.connection_pin.release();
    worker.join();
    completed.get();
    CHECK(devices.registry().resident_count() == 0U);
}

TEST_CASE("GetState neutral reads retain an unpublished connection without reading a sample",
    "[component][xinput]") {
    const bool background = GENERATE(false, true);
    focus_fixture focus_state;
    registry_fixture devices;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    std::unique_ptr<source_lifetime> source = std::make_unique<test_source_lifetime>();
    const auto connection = devices.registry().admit(source);
    REQUIRE(connection.has_value());
    const auto occupant = service.assign(connection->target, connection->reader);
    REQUIRE(occupant.has_value());
    if (background) {
        focus_state.set(focus::evidence::background);
    } else {
        service.enable(FALSE);
    }
    auto state = sentinel_state();
    CHECK(service.get_state(xinput::flavor::v1_4, occupant->user_index, &state) == ERROR_SUCCESS);
    CHECK(state.dwPacketNumber == 0U);
    CHECK(state.Gamepad.wButtons == 0U);
    service.enable(TRUE);
    focus_state.set(focus::evidence::foreground);
    state = sentinel_state();
    CHECK(service.get_state(xinput::flavor::v1_4, occupant->user_index, &state) ==
          ERROR_DEVICE_NOT_CONNECTED);
    CHECK(state.dwPacketNumber == sentinel_state().dwPacketNumber);
    CHECK(state.Gamepad.wButtons == sentinel_state().Gamepad.wButtons);
}

} // namespace
