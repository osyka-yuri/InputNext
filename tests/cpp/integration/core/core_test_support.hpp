#pragma once

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "focus/observer_test_access.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "support/focus/manual_clock.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/xinput.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::core_test_support {

using namespace std::chrono_literals;
using namespace inputnext::gameinput_test_support;
using namespace inputnext::gameinput_detail;

template <typename Value>
[[nodiscard]] const Value& required_value(const std::optional<Value>& value) {
    if (!value.has_value()) {
        throw std::runtime_error{"test fixture value was not produced"};
    }
    return value.value();
}

} // namespace inputnext::core_test_support

namespace inputnext::core_test_support {

class xinput_metadata_source final : public xinput::metadata_source {
public:
    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const connection_target target) override {
        static_cast<void>(target);
        return std::unexpected(metadata_error::not_connected);
    }
};

class switchable_focus_source final : public focus::source {
public:
    explicit switchable_focus_source(const focus::process_instance process) noexcept
        : process_(process) {}

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        return focus::reading{.process = process_, .state = state_.load(std::memory_order_acquire)};
    }

    void set_state(const focus::evidence state) noexcept {
        state_.store(state, std::memory_order_release);
    }

private:
    focus::process_instance process_{};
    std::atomic<focus::evidence> state_{focus::evidence::foreground};
};

class history_call_release_guard final {
public:
    explicit history_call_release_guard(std::shared_ptr<native_history_script> script) noexcept
        : script_(std::move(script)) {}

    ~history_call_release_guard() { release(); }

    history_call_release_guard(const history_call_release_guard&) = delete;
    history_call_release_guard& operator=(const history_call_release_guard&) = delete;
    history_call_release_guard(history_call_release_guard&&) = delete;
    history_call_release_guard& operator=(history_call_release_guard&&) = delete;

    void release() noexcept {
        if (script_ != nullptr) {
            script_->release_blocked_call();
            script_.reset();
        }
    }

private:
    std::shared_ptr<native_history_script> script_;
};

[[nodiscard]] inline metadata_field<metadata_device_model> available_model(
    const metadata_device_model model) noexcept {
    return metadata_field<metadata_device_model>{.availability = metadata_availability::available,
        .value = model};
}

[[nodiscard]] inline metadata_field<transport_metadata> available_transport(
    const physical_transport transport,
    const transport_provenance provenance) noexcept {
    return metadata_field<transport_metadata>{.availability = metadata_availability::available,
        .value = {.value = transport, .provenance = provenance}};
}

[[nodiscard]] inline metadata_field<opaque_device_id> available_id(
    std::uint8_t first_byte) noexcept {
    opaque_device_id id{};
    id.bytes[0U] = static_cast<std::byte>(first_byte);
    return metadata_field<opaque_device_id>{.availability = metadata_availability::available,
        .value = id};
}

class fixture final {
public:
    fixture() {
        gameinput.metadata->set_observation(available_model(metadata_device_model::generic_gamepad),
            available_transport(physical_transport::unknown, transport_provenance::unavailable),
            false);
        auto facts = runtime_fixture::native_facts(u"unused");
        facts.gameinput_device_id = available_id(1U);
        facts.gameinput_root_id = available_id(2U);
        facts.gameinput_capabilities = metadata_field<native_gameinput_capabilities>{
            .availability = metadata_availability::available,
            .value = {.gamepad = true, .low_frequency_rumble = true}};
        gameinput.native->update_facts(std::move(facts));

        const auto script = std::make_shared<native_history_script>();
        scripts[0U] = script;
        gameinput.native->configure_history(handle(gameinput.device_memory[0U]), script);
        if (!gameinput.start().has_value()) {
            throw std::runtime_error{"GameInput test runtime failed to start"};
        }

        const auto process = focus::process_instance{.pid = 321U, .creation_time_100ns = 987654U};
        auto source = std::make_unique<switchable_focus_source>(process);
        focus_source = source.get();
        focus_observer = focus::detail::observer_test_access::make_with_clock(focus_identity(),
            std::move(source),
            &test::focus::manual_clock::read,
            &clock);
        wait_for_foreground();
        presentation = std::make_unique<xinput::service>(*focus_observer, xinput_metadata);
        core = std::make_unique<inputnext::core::service>(test_runtime_id(),
            scripts[0U]->qpc_frequency,
            gameinput.registry,
            *gameinput.runtime,
            *focus_observer,
            *presentation);
    }

    ~fixture() {
        release_history_calls();
        if (core) {
            core->close_admission();
        }
        if (presentation) {
            presentation->close();
        }
        if (gameinput.runtime) {
            static_cast<void>(gameinput.runtime->stop(inputnext::maintenance_time::clock::now()));
        }
        if (focus_observer) {
            focus_observer->stop();
        }
        core.reset();
        presentation.reset();
        focus_observer.reset();
        gameinput.registry.shutdown();
    }

    fixture(const fixture&) = delete;
    fixture& operator=(const fixture&) = delete;
    fixture(fixture&&) = delete;
    fixture& operator=(fixture&&) = delete;

    [[nodiscard]] connection_target connect(std::size_t index = 0U,
        const bool publish_sample = true) {
        if (index >= gameinput.device_memory.size()) {
            throw std::out_of_range{"test device index exceeds fixture capacity"};
        }
        const auto native = handle(gameinput.device_memory[index]);
        if (scripts[index] == nullptr) {
            scripts[index] = std::make_shared<native_history_script>();
        }
        gameinput.native->configure_history(native, scripts[index]);
        if (publish_sample) {
            gameinput.native->set_current(native,
                native_reading_lane::gamepad,
                pad_parts(test_pad(3U), 120U, 1'200U));
            gameinput.native->set_current(native,
                native_reading_lane::system_button,
                reading_parts{.guide = false,
                    .source_timestamp_us = 120U,
                    .observation_qpc = 1'200U});
        }

        const auto before = gameinput.runtime->connections().size;
        gameinput.native->emit(native, native_device_event::connected);
        for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
            gameinput.maintain();
            const auto connections = gameinput.runtime->connections();
            if (connections.size > before && index < connections.size &&
                (!publish_sample || connections.entries[index].pad_known)) {
                return connections.entries[index].target;
            }
        }
        throw std::runtime_error{"GameInput fixture failed to admit a test device"};
    }

    void assign_xinput(const connection_target target) const {
        const auto connections = gameinput.runtime->connections();
        const auto found =
            std::ranges::find_if(std::span{connections.entries}.first(connections.size),
                [target](const gameinput_connection& connection) {
                    return connection.target == target;
                });
        if (found == std::span{connections.entries}.first(connections.size).end()) {
            throw std::runtime_error{"target is absent from the GameInput fixture"};
        }
        if (!presentation->assign(target, found->reader).has_value()) {
            throw std::runtime_error{"XInput fixture failed to assign target"};
        }
    }

    void set_model_transport(const metadata_device_model model,
        const physical_transport transport,
        const transport_provenance provenance,
        const bool conflicting = false) const {
        gameinput.metadata->set_observation(available_model(model),
            available_transport(transport, provenance),
            conflicting);
    }

    void set_focus(const focus::evidence state) const noexcept { focus_source->set_state(state); }

    [[nodiscard]] bool wait_for_focus(const focus::evidence state,
        const std::chrono::milliseconds timeout = 2000ms) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (focus_observer->published().state == state) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void release_history_calls() noexcept {
        for (const auto& script : scripts) {
            if (script != nullptr) {
                script->release_blocked_call();
            }
        }
    }

    gameinput_test_support::runtime_fixture gameinput;
    std::array<std::shared_ptr<native_history_script>, 16U> scripts{};
    xinput_metadata_source xinput_metadata;
    switchable_focus_source* focus_source{};
    test::focus::manual_clock clock;
    std::unique_ptr<focus::observer> focus_observer;
    std::unique_ptr<xinput::service> presentation;
    std::unique_ptr<inputnext::core::service> core;

private:
    [[nodiscard]] static runtime_id focus_identity() noexcept {
        auto identity = test_runtime_id();
        identity.bytes[0U] = std::byte{0x52};
        return identity;
    }

    void wait_for_foreground() const {
        for (std::size_t attempt = 0U; attempt < 2000U; ++attempt) {
            bool ready{};
            {
                const auto guard = focus_observer->lock_presentation();
                ready = guard.current_foreground_epoch().has_value();
            }
            if (ready) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error{"focus fixture failed to publish foreground"};
    }
};

} // namespace inputnext::core_test_support
