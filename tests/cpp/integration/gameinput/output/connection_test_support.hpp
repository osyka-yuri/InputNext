#pragma once

#include "cpp/integration/gameinput/runtime/runtime_rumble_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "output/composition/composition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <gameinput/runtime/output_access.hpp>

#include <output/authority_fixture.hpp>

namespace inputnext::gameinput_detail::test_support {
using namespace std::chrono_literals;
using namespace inputnext::gameinput_test_support;

template <typename Value>
[[nodiscard]] Value checked_value(const std::optional<Value>& candidate) {
    if (!candidate.has_value()) {
        throw std::runtime_error("Expected a populated connection test value.");
    }
    return candidate.value();
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

class release_rumble_controls final {
public:
    explicit release_rumble_controls(std::shared_ptr<rumble_test_controls> controls) noexcept
        : controls_(std::move(controls)) {}

    ~release_rumble_controls() noexcept { controls_->release_all(); }

    release_rumble_controls(const release_rumble_controls&) = delete;
    release_rumble_controls& operator=(const release_rumble_controls&) = delete;
    release_rumble_controls(release_rumble_controls&&) = delete;
    release_rumble_controls& operator=(release_rumble_controls&&) = delete;

private:
    std::shared_ptr<rumble_test_controls> controls_;
};

class connection_fixture final {
public:
    connection_fixture() = default;

    ~connection_fixture() noexcept {
        if (controls != nullptr) {
            controls->release_all();
        }
        if (connections != nullptr) {
            connections->close_admission();
        }
        if (presentation != nullptr) {
            presentation->close();
        }
        if (connections != nullptr) {
            connections->shutdown();
        }
        if (runtime_started) {
            static_cast<void>(backing.runtime->stop(backing.now));
        }
        if (focus_observer != nullptr) {
            focus_observer->stop();
        }
    }

    connection_fixture(const connection_fixture&) = delete;
    connection_fixture& operator=(const connection_fixture&) = delete;
    connection_fixture(connection_fixture&&) = delete;
    connection_fixture& operator=(connection_fixture&&) = delete;

    [[nodiscard]] bool start(const bool synchronize_output = true,
        const bool seed_pad_for_presentation = false) {
        controls = backing.native->rumble_script();
        backing.native->device_facts.gameinput_capabilities =
            metadata_field<native_gameinput_capabilities>{.availability =
                                                              metadata_availability::available,
                .value = native_gameinput_capabilities{.gamepad = true,
                    .low_frequency_rumble = true,
                    .high_frequency_rumble = true,
                    .left_trigger_rumble = false,
                    .right_trigger_rumble = false}};

        auto output_access = runtime_output_access::create(*backing.runtime);
        if (!output_access.has_value()) {
            return false;
        }
        runtime_access = std::move(*output_access);

        const auto process =
            focus::process_instance{.pid = 723U, .creation_time_100ns = 1'234'567U};
        auto source = std::make_unique<test::output::foreground_source>(process);
        focus_observer = std::make_unique<focus::observer>(focus_incarnation(), std::move(source));
        if (!wait_until([this] {
                const auto observed = focus_observer->published();
                return observed.open && observed.state == focus::evidence::foreground;
            })) {
            return false;
        }

        composition = std::make_unique<output::detail::composition>(test_runtime_id(),
            backing.registry,
            *focus_observer);
        auto created_executor = output::executor::create(composition->public_authority());
        if (!created_executor.has_value()) {
            return false;
        }
        output_executor = std::move(*created_executor);
        auto created_connections =
            gameinput_output_connections::create(*runtime_access, *output_executor);
        if (!created_connections.has_value()) {
            return false;
        }
        connections = std::move(*created_connections);

        auto rumble = composition->make_xinput_rumble();
        if (!rumble.has_value()) {
            return false;
        }
        presentation = std::make_unique<gameinput_xinput>(*backing.runtime,
            *focus_observer,
            10'000'000U,
            std::move(*rumble));

        if (!backing.start().has_value()) {
            return false;
        }
        runtime_started = true;
        device = handle(backing.device_memory[0U]);
        backing.connect(device);
        backing.maintain();
        if (backing.connection_count() != 1U) {
            return false;
        }
        target = backing.connection().target;
        if (seed_pad_for_presentation) {
            backing.native->set_current(device,
                native_reading_lane::gamepad,
                pad_parts(test_pad(button_mask{}), 1U, 1U));
            backing.maintain();
        }
        const auto inventory = backing.runtime->connections();
        return (!synchronize_output || connections->synchronize(inventory).has_value()) &&
               presentation->synchronize(inventory).has_value();
    }

    [[nodiscard]] bool wait_for_backend(connection_target expected) const {
        return wait_until([this, expected] {
            const auto state = connections->backend_progress(expected);
            return state.has_value() && state->phase == output::backend_phase::current &&
                   state->backend.valid();
        });
    }

    [[nodiscard]] static runtime_id focus_incarnation() noexcept {
        auto result = test_runtime_id();
        result.bytes[1U] = std::byte{0x72};
        return result;
    }

    runtime_fixture backing;
    std::shared_ptr<rumble_test_controls> controls;
    std::unique_ptr<focus::observer> focus_observer;
    std::unique_ptr<output::detail::composition> composition;
    std::unique_ptr<output::executor> output_executor;
    std::unique_ptr<runtime_output_access> runtime_access;
    std::unique_ptr<gameinput_output_connections> connections;
    std::unique_ptr<gameinput_xinput> presentation;
    native_device_handle device{};
    connection_target target{};
    bool runtime_started{};
};

} // namespace inputnext::gameinput_detail::test_support
