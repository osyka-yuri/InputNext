#pragma once

#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_rumble_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/reading_parts.hpp"
#include "metadata/provider.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include "support/focus/controlled_source.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <Windows.h>
#include <profileapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace inputnext::runtime_session_test_support {

using namespace inputnext;
using namespace inputnext::gameinput_test_support;
using namespace inputnext::gameinput_detail;
using namespace inputnext::runtime_detail;
using namespace std::chrono_literals;

constexpr auto wait_period = 1ms;
constexpr auto wait_limit = 2s;

struct dependency_destruction_counts final {
    std::atomic<std::size_t> native;
    std::atomic<std::size_t> metadata;
    std::atomic<std::size_t> focus;
};

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
    const std::chrono::steady_clock::duration timeout = wait_limit) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(wait_period);
    }
    return predicate();
}

[[nodiscard]] inline std::uint64_t current_qpc() {
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value) == FALSE || value.QuadPart <= 0) {
        throw std::runtime_error{"QueryPerformanceCounter failed in session test"};
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] inline std::uint64_t current_qpc_frequency() {
    LARGE_INTEGER value{};
    if (QueryPerformanceFrequency(&value) == FALSE || value.QuadPart <= 0) {
        throw std::runtime_error{"QueryPerformanceFrequency failed in session test"};
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

class counted_native_port final : public native_gameinput {
public:
    explicit counted_native_port(std::shared_ptr<std::atomic<std::size_t>> initialize_calls,
        std::shared_ptr<dependency_destruction_counts> destruction_counts)
        : fake(std::make_unique<fake_gameinput>()),
          initialize_calls_(std::move(initialize_calls)),
          destruction_counts_(std::move(destruction_counts)) {}

    ~counted_native_port() override {
        destruction_counts_->native.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] native_result<void> initialize(
        const std::filesystem::path& runtime_library) override {
        initialize_calls_->fetch_add(1U, std::memory_order_relaxed);
        return fake->initialize(runtime_library);
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        return fake->register_device_watch(context);
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        return fake->register_device_rescan(context);
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        const native_device_handle device,
        native_reading_callback_context& context) override {
        return fake->register_reading_callback(device, context);
    }

    [[nodiscard]] bool unregister_callback(const native_callback_token token) noexcept override {
        if (unregister_always_fails.load(std::memory_order_acquire)) {
            return false;
        }
        return fake->unregister_callback(token);
    }

    [[nodiscard]] native_result<reading_parts> current(const native_device_handle device,
        const native_reading_lane lane) override {
        return fake->current(device, lane);
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        const native_device_handle device) override {
        return fake->create_history_cursor(device);
    }

    [[nodiscard]] native_result<std::uint64_t> performance_frequency() noexcept override {
        return fake->performance_frequency();
    }

    [[nodiscard]] native_result<native_device_facts> facts(
        const native_device_handle device) override {
        if (throw_next_facts.exchange(false, std::memory_order_acq_rel)) {
            throw std::runtime_error{"injected native facts failure"};
        }
        return fake->facts(device);
    }

    [[nodiscard]] bool connected(const native_device_handle device) noexcept override {
        return fake->connected(device);
    }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>> create_rumble_endpoint(
        const native_device_handle device) override {
        return fake->create_rumble_endpoint(device);
    }

    void retain(const native_device_handle device) noexcept override { fake->retain(device); }

    void release(const native_device_handle device) noexcept override { fake->release(device); }

    std::unique_ptr<fake_gameinput> fake;
    std::atomic<bool> throw_next_facts;
    std::atomic<bool> unregister_always_fails;

    counted_native_port(const counted_native_port&) = delete;
    counted_native_port& operator=(const counted_native_port&) = delete;
    counted_native_port(counted_native_port&&) = delete;
    counted_native_port& operator=(counted_native_port&&) = delete;

private:
    std::shared_ptr<std::atomic<std::size_t>> initialize_calls_;
    std::shared_ptr<dependency_destruction_counts> destruction_counts_;
};

class counted_metadata_provider final : public metadata_provider {
public:
    explicit counted_metadata_provider(
        std::shared_ptr<dependency_destruction_counts> destruction_counts) noexcept
        : destruction_counts_(std::move(destruction_counts)) {}

    counted_metadata_provider(const counted_metadata_provider&) = delete;
    counted_metadata_provider& operator=(const counted_metadata_provider&) = delete;
    counted_metadata_provider(counted_metadata_provider&&) = delete;
    counted_metadata_provider& operator=(counted_metadata_provider&&) = delete;

    ~counted_metadata_provider() override {
        destruction_counts_->metadata.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] metadata_result<metadata_snapshot> query(
        const metadata_request& request) override {
        metadata_snapshot result{};
        result.target = request.target;
        result.catalogue_generation = request.catalogue_generation;
        result.native = request.facts;
        result.container_id = request.facts.container_id;
        result.container_id_source = request.facts.container_id.has_value()
                                         ? container_id_provenance::gameinput_native
                                         : container_id_provenance::unavailable;
        return result;
    }

private:
    std::shared_ptr<dependency_destruction_counts> destruction_counts_;
};

class session_fixture final {
public:
    explicit session_fixture(worker_launcher launcher = nullptr, void* launch_context = nullptr)
        : destruction_counts(std::make_shared<dependency_destruction_counts>()),
          initialize_calls(std::make_shared<std::atomic<std::size_t>>(0U)) {
        auto port = std::make_unique<counted_native_port>(initialize_calls, destruction_counts);
        native = port.get();
        native->fake->device_facts = runtime_fixture::native_facts(u"session-endpoint");
        native->fake->device_facts.vendor_id.value = 0x045EU;
        native->fake->device_facts.product_id.value = 0x02FDU;
        native->fake->device_facts.gameinput_capabilities =
            metadata_field<native_gameinput_capabilities>{
                .availability = metadata_availability::available,
                .value = native_gameinput_capabilities{.gamepad = true,
                    .low_frequency_rumble = true,
                    .high_frequency_rumble = true},
        };
        rumble_controls = native->fake->rumble_script();

        native_session_dependencies options{};
        options.identity = test_runtime_id();
        options.identity.bytes[1U] = std::byte{0xA7U};
        options.runtime_library = L"C:\\fixture\\GameInputRedist.dll";
        options.native = std::move(port);
        options.metadata = std::make_unique<counted_metadata_provider>(destruction_counts);
        const auto process =
            focus::process_instance{.pid = 731U, .creation_time_100ns = 4'567'890U};
        options.focus_source = std::make_unique<test::focus::controlled_source>(process,
            std::vector<focus::reading>{
                {.process = process, .state = focus::evidence::foreground},
            },
            0U,
            &destruction_counts->focus);
        options.launch_worker = launcher;
        options.launch_context = launch_context;
        try {
            session = native_session::prepare(std::move(options));
        } catch (...) {
            preparation_error = std::current_exception();
            native = nullptr;
        }
    }

    ~session_fixture() {
        if (session != nullptr) {
            release_blocked_work();
            session->shutdown();
        }
    }

    session_fixture(const session_fixture&) = delete;
    session_fixture& operator=(const session_fixture&) = delete;
    session_fixture(session_fixture&&) = delete;
    session_fixture& operator=(session_fixture&&) = delete;

    void configure_device(const std::shared_ptr<native_history_script>& script,
        const button_mask buttons = static_cast<button_mask>(gamepad_button::south)) {
        configure_device(script, test_pad(buttons));
    }

    void configure_device(const std::shared_ptr<native_history_script>& script,
        const gamepad_state state) {
        history = script;
        history->qpc_frequency = current_qpc_frequency();
        history->seed.qpc_frequency = history->qpc_frequency;
        history->seed.baseline = input_update{
            .state = state,
            .source_timestamp_us = 100U,
            .observation_qpc = current_qpc(),
        };
        device = handle(device_memory[0U]);
        native->fake->set_current(device,
            native_reading_lane::gamepad,
            pad_parts(state, 100U, history->seed.baseline.observation_qpc));
        native->fake->set_current(device,
            native_reading_lane::system_button,
            reading_parts{.guide = false,
                .source_timestamp_us = 100U,
                .observation_qpc = history->seed.baseline.observation_qpc});
        native->fake->configure_history(device, history);
    }

    [[nodiscard]] gameinput_result<void> start() const {
        auto result = session->start();
        if (result.has_value() && device.valid()) {
            native->fake->emit(device, native_device_event::connected);
        }
        return result;
    }

    [[nodiscard]] bool wait_for_presentation() const {
        return wait_until([this] { return session->presentation().positions()[0U].has_value(); });
    }

    [[nodiscard]] bool wait_for_output_endpoint() const {
        return wait_until([this] { return rumble_controls->factory_calls() > 0U; });
    }

    void block_next_facts() noexcept {
        facts_blocked_armed = true;
        native->fake->block_next_facts();
    }

    void release_facts() noexcept {
        if (facts_blocked_armed) {
            native->fake->release_facts();
            facts_blocked_armed = false;
        }
    }

    [[nodiscard]] DWORD read_state(XINPUT_STATE& state) const {
        return session->presentation().get_state(xinput::flavor::v1_4, 0U, &state);
    }

    [[nodiscard]] bool wait_for_button(const WORD button) const {
        return wait_until([this, button] {
            XINPUT_STATE state{};
            return read_state(state) == ERROR_SUCCESS &&
                   (state.Gamepad.wButtons & button) == button;
        });
    }

    void release_blocked_work() noexcept {
        if (history != nullptr) {
            history->release_blocked_call();
        }
        release_facts();
        if (rumble_controls != nullptr) {
            rumble_controls->release_all();
        }
    }

    std::array<int, 2U> device_memory{};
    std::shared_ptr<dependency_destruction_counts> destruction_counts;
    std::shared_ptr<std::atomic<std::size_t>> initialize_calls;
    counted_native_port* native{};
    std::shared_ptr<rumble_test_controls> rumble_controls;
    std::shared_ptr<native_history_script> history;
    native_device_handle device{};
    std::unique_ptr<native_session> session;
    std::exception_ptr preparation_error;
    bool facts_blocked_armed{};
};

} // namespace inputnext::runtime_session_test_support
