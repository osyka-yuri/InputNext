#include "runtime_api_test_composition.hpp"
#include "audio/audio_service.hpp"

#include "audio/unavailable_native_factory.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "runtime/abi/runtime_api.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/bootstrap/bootstrap_owner.hpp"
#include "runtime/bootstrap/query_front.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/output.hpp>

#include <Windows.h>
#include <profileapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace inputnext::runtime_detail {
namespace {

using gameinput_detail::native_device_event;
using gameinput_detail::native_device_handle;
using gameinput_detail::native_reading_lane;
using gameinput_detail::reading_parts;
using gameinput_test_support::fake_gameinput;
using gameinput_test_support::fake_metadata_provider;
using gameinput_test_support::native_history_script;
using gameinput_test_support::pad_parts;
using gameinput_test_support::runtime_fixture;
using gameinput_test_support::test_pad;
using gameinput_test_support::test_runtime_id;

constexpr focus::process_instance fixture_process{.pid = 731U, .creation_time_100ns = 4'567'890U};
constinit std::atomic<void*> active_composition{};
constinit query_front no_composition_front{query_front_operations{}, nullptr};

class fixture_focus_source final : public focus::source {
public:
    fixture_focus_source(std::atomic<std::uint32_t>& observations,
        const focus::process_instance process) noexcept
        : observations_{observations},
          process_{process} {}

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        observations_.fetch_add(1U, std::memory_order_relaxed);
        return {.process = process_, .state = focus::evidence::foreground};
    }

private:
    std::atomic<std::uint32_t>& observations_;
    const focus::process_instance process_;
};

class session_focus_source final : public focus::source {
public:
    [[nodiscard]] focus::process_instance process() const noexcept override {
        return fixture_process;
    }

    [[nodiscard]] focus::reading observe() noexcept override {
        return {.process = fixture_process, .state = focus::evidence::foreground};
    }
};

} // namespace

bool bootstrap_owner_test_access::control_gate_available(bootstrap_owner& owner) {
    std::unique_lock lock{owner.control_gate_, std::try_to_lock};
    return lock.owns_lock();
}

struct runtime_api_test_fixture::implementation final {
    explicit implementation(const bool fail_start) noexcept
        : fail_native_start{fail_start},
          front{query_front_operations{.pin_module = &pin_module,
                    .create_owner = &create_owner,
                    .stable_api = &stable_api},
              this} {}

    [[nodiscard]] std::unique_ptr<native_session> prepare_session() {
        prepare_attempts.fetch_add(1U, std::memory_order_relaxed);
        {
            std::unique_lock lock{prepare_gate};
            if (block_prepare) {
                prepare_entered = true;
                prepare_changed.notify_all();
                prepare_changed.wait(lock, [this] { return prepare_released; });
                block_prepare = false;
                prepare_entered = false;
                prepare_released = false;
            }
        }

        native_session_dependencies dependencies{};
        dependencies.audio_factory = inputnext::test::audio::make_unavailable_native_port_factory();
        dependencies.audio_launcher = inputnext::audio_detail::make_std_audio_worker_launcher();
        dependencies.identity = test_runtime_id();
        dependencies.identity.bytes[1U] = std::byte{0xA7U};
        dependencies.runtime_library = L"C:\\fixture\\GameInputRedist.dll";

        auto native_owner = std::make_unique<fake_gameinput>();
        auto* const native_view = native_owner.get();
        native_view->device_facts = runtime_fixture::native_facts(u"adapter-endpoint");
        native_view->device_facts.vendor_id.value = 0x045EU;
        native_view->device_facts.product_id.value = 0x02FDU;
        native_view->device_facts.gameinput_capabilities =
            metadata_field<native_gameinput_capabilities>{
                .availability = metadata_availability::available,
                .value = native_gameinput_capabilities{.gamepad = true,
                    .low_frequency_rumble = true,
                    .high_frequency_rumble = true},
        };
        if (fail_native_start) {
            native_view->fail_next_watch_registration_with_partial_token();
        }
        LARGE_INTEGER qpc_frequency{};
        if (QueryPerformanceFrequency(&qpc_frequency) == FALSE || qpc_frequency.QuadPart <= 0) {
            throw std::runtime_error{"QueryPerformanceFrequency failed in runtime API fixture"};
        }
        LARGE_INTEGER observation_qpc{};
        if (QueryPerformanceCounter(&observation_qpc) == FALSE || observation_qpc.QuadPart <= 0) {
            throw std::runtime_error{"QueryPerformanceCounter failed in runtime API fixture"};
        }
        const auto history = std::make_shared<native_history_script>();
        history->qpc_frequency = static_cast<std::uint64_t>(qpc_frequency.QuadPart);
        history->seed.qpc_frequency = history->qpc_frequency;
        history->seed.baseline = input_update{
            .state = test_pad(static_cast<button_mask>(gamepad_button::south)),
            .source_timestamp_us = 100U,
            .observation_qpc = static_cast<std::uint64_t>(observation_qpc.QuadPart),
        };
        const native_device_handle fixture_device{&device_storage};
        device = fixture_device;
        native_view->set_current(fixture_device,
            native_reading_lane::gamepad,
            pad_parts(history->seed.baseline.state, 100U, history->seed.baseline.observation_qpc));
        native_view->set_current(fixture_device,
            native_reading_lane::system_button,
            reading_parts{.guide = false,
                .source_timestamp_us = 100U,
                .observation_qpc = history->seed.baseline.observation_qpc});
        native_view->configure_history(fixture_device, history);

        dependencies.native = std::move(native_owner);
        dependencies.metadata = std::make_unique<fake_metadata_provider>();
        dependencies.focus_source = std::make_unique<session_focus_source>();
        auto session = native_session::prepare(std::move(dependencies));
        native.store(native_view, std::memory_order_release);
        {
            std::unique_lock lock{publication_gate};
            if (block_publication) {
                prepared_presentation = &session->presentation();
                prepared_presentation_ready = true;
                publication_changed.notify_all();
                publication_changed.wait(lock, [this] { return publication_released; });
                publication_factory_resumed = true;
                publication_changed.notify_all();
            }
        }
        return session;
    }

    static BOOL pin_module(void* const context) noexcept {
        auto& self = *static_cast<implementation*>(context);
        self.pin_calls.fetch_add(1U, std::memory_order_relaxed);
        return TRUE;
    }

    static bootstrap_owner* create_owner(void* const context) {
        auto& self = *static_cast<implementation*>(context);
        self.owner_creations.fetch_add(1U, std::memory_order_relaxed);
        self.owner = std::make_unique<bootstrap_owner>(
            std::make_unique<fixture_focus_source>(self.focus_observations, fixture_process),
            [](void* const borrowed_context) {
                return static_cast<implementation*>(borrowed_context)->prepare_session();
            },
            &self);
        return self.owner.get();
    }

    static const input_next_runtime_api_v1* stable_api([[maybe_unused]] void* context,
        bootstrap_owner* const owner) noexcept {
        return owner != nullptr ? runtime_api_v1() : nullptr;
    }

    const bool fail_native_start;
    std::atomic<std::uint32_t> pin_calls;
    std::atomic<std::uint32_t> owner_creations;
    std::atomic<std::uint32_t> focus_observations;
    std::atomic<std::uint32_t> prepare_attempts;
    std::atomic<fake_gameinput*> native;
    int device_storage{};
    native_device_handle device{};
    std::mutex prepare_gate;
    std::condition_variable prepare_changed;
    bool block_prepare{};
    bool prepare_entered{};
    bool prepare_released{};
    std::mutex publication_gate;
    std::condition_variable publication_changed;
    bool block_publication{};
    bool publication_released{};
    bool prepared_presentation_ready{};
    bool publication_factory_resumed{};
    inputnext::xinput::service* prepared_presentation{};
    query_front front;
    std::unique_ptr<bootstrap_owner> owner;
};

runtime_api_test_fixture::runtime_api_test_fixture(const bool fail_native_start)
    : implementation_{std::make_unique<implementation>(fail_native_start)} {
    void* expected{};
    if (!active_composition.compare_exchange_strong(expected,
            implementation_.get(),
            std::memory_order_release,
            std::memory_order_relaxed)) {
        throw std::logic_error{"runtime API test composition is already bound"};
    }
}

runtime_api_test_fixture::~runtime_api_test_fixture() {
    void* expected = implementation_.get();
    if (!active_composition.compare_exchange_strong(expected,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        std::terminate();
    }
    implementation_.reset();
}

void runtime_api_test_fixture::block_next_prepare() {
    std::scoped_lock lock{implementation_->prepare_gate};
    implementation_->block_prepare = true;
    implementation_->prepare_entered = false;
    implementation_->prepare_released = false;
}

bool runtime_api_test_fixture::wait_for_prepare(const std::chrono::milliseconds timeout) {
    std::unique_lock lock{implementation_->prepare_gate};
    return implementation_->prepare_changed.wait_for(lock, timeout, [this] {
        return implementation_->prepare_entered;
    });
}

void runtime_api_test_fixture::release_prepare() {
    {
        std::scoped_lock lock{implementation_->prepare_gate};
        implementation_->prepare_released = true;
    }
    implementation_->prepare_changed.notify_all();
}

void runtime_api_test_fixture::block_before_publication() {
    std::scoped_lock lock{implementation_->publication_gate};
    implementation_->block_publication = true;
    implementation_->publication_released = false;
    implementation_->prepared_presentation_ready = false;
    implementation_->publication_factory_resumed = false;
    implementation_->prepared_presentation = nullptr;
}

inputnext::xinput::service* runtime_api_test_fixture::wait_for_prepared_presentation(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{implementation_->publication_gate};
    if (!implementation_->publication_changed.wait_for(lock, timeout, [this] {
            return implementation_->prepared_presentation_ready;
        })) {
        return nullptr;
    }
    return implementation_->prepared_presentation;
}

void runtime_api_test_fixture::release_publication_factory() {
    {
        std::scoped_lock lock{implementation_->publication_gate};
        implementation_->publication_released = true;
    }
    implementation_->publication_changed.notify_all();
}

bool runtime_api_test_fixture::wait_for_publication_factory_resume(
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{implementation_->publication_gate};
    return implementation_->publication_changed.wait_for(lock, timeout, [this] {
        return implementation_->publication_factory_resumed;
    });
}

void runtime_api_test_fixture::connect_native_device() {
    auto* const native = implementation_->native.load(std::memory_order_acquire);
    if (native == nullptr || !implementation_->device.valid()) {
        throw std::logic_error{"runtime API test device is not prepared"};
    }
    native->emit(implementation_->device, native_device_event::connected);
}

std::uint32_t runtime_api_test_fixture::pin_calls() const noexcept {
    return implementation_->pin_calls.load(std::memory_order_acquire);
}

std::uint32_t runtime_api_test_fixture::owner_creations() const noexcept {
    return implementation_->owner_creations.load(std::memory_order_acquire);
}

std::uint32_t runtime_api_test_fixture::focus_observations() const noexcept {
    return implementation_->focus_observations.load(std::memory_order_acquire);
}

std::uint32_t runtime_api_test_fixture::prepare_attempts() const noexcept {
    return implementation_->prepare_attempts.load(std::memory_order_acquire);
}

bool runtime_api_test_fixture::native_initialized() const noexcept {
    const auto* const native = implementation_->native.load(std::memory_order_acquire);
    return native != nullptr && native->initialized;
}

bool runtime_api_test_fixture::owner_is_ready() const noexcept {
    const auto* const owner = implementation_->front.owner.load(std::memory_order_acquire);
    return owner != nullptr && owner->state() == bootstrap_state::ready;
}

bool runtime_api_test_fixture::owner_is_degraded() const noexcept {
    const auto* const owner = implementation_->front.owner.load(std::memory_order_acquire);
    return owner != nullptr && owner->state() == bootstrap_state::degraded;
}

std::size_t runtime_api_test_fixture::rumble_writes() const noexcept {
    const auto* const native = implementation_->native.load(std::memory_order_acquire);
    return native != nullptr ? native->rumble_script()->writes() : 0U;
}

std::optional<output::rumble_value> runtime_api_test_fixture::last_rumble_value() const {
    const auto* const native = implementation_->native.load(std::memory_order_acquire);
    return native != nullptr ? native->rumble_script()->last_value() : std::nullopt;
}

query_front& runtime_query_front() noexcept {
    auto* const context = active_composition.load(std::memory_order_acquire);
    return context != nullptr
               ? static_cast<runtime_api_test_fixture::implementation*>(context)->front
               : no_composition_front;
}

bootstrap_owner* runtime_bootstrap_owner() noexcept {
    auto* const context = active_composition.load(std::memory_order_acquire);
    if (context == nullptr) {
        return nullptr;
    }
    auto const& front = static_cast<runtime_api_test_fixture::implementation*>(context)->front;
    return front.owner.load(std::memory_order_acquire);
}

} // namespace inputnext::runtime_detail
