#include "audio/audio_service.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"

#include "audio/unavailable_native_factory.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "runtime/bootstrap/bootstrap_owner.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime/session/native_session_dependencies.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::runtime_detail;
using namespace inputnext::gameinput_test_support;

class controlled_focus_source final : public focus::source {
public:
    [[nodiscard]] focus::process_instance process() const noexcept override {
        return {.pid = 8642U, .creation_time_100ns = 24680U};
    }

    [[nodiscard]] focus::reading observe() noexcept override {
        ++observations_;
        return {.process = process(), .state = state_.load(std::memory_order_relaxed)};
    }

    void set(const focus::evidence state) noexcept {
        state_.store(state, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t observations() const noexcept {
        return observations_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<focus::evidence> state_{focus::evidence::foreground};
    std::atomic<std::uint32_t> observations_;
};

struct session_factory_state final {
    bootstrap_owner* owner{};
    std::atomic<std::uint32_t> attempts;
    std::atomic<bool> reenter_owner;
    std::atomic<bool> reentry_returned_null;
    std::atomic<bool> fail_native_start;
    std::atomic<bool> fail_worker_launch_once;
    std::atomic<bool> rollback_completed;
    std::atomic<bool> retry_after_rollback;
    std::atomic<fake_gameinput*> native;
    std::mutex gate;
    std::condition_variable changed;
    bool block_factory_once{};
    bool factory_entered{};
    bool factory_released{};
    bool worker_launcher_entered{};
    bool worker_launcher_released{};
};

[[nodiscard]] bool wait_for_flag(std::mutex& gate,
    std::condition_variable& changed,
    const bool& flag,
    const std::chrono::milliseconds timeout) {
    std::unique_lock lock{gate};
    return changed.wait_for(lock, timeout, [&flag] { return flag; });
}

void release_flag(std::mutex& gate, std::condition_variable& changed, bool& flag) {
    {
        std::scoped_lock lock{gate};
        flag = true;
    }
    changed.notify_all();
}

struct release_flag_on_exit final {
    std::mutex& gate;
    std::condition_variable& changed;
    bool& flag;

    release_flag_on_exit(std::mutex& gate_value,
        std::condition_variable& changed_value,
        bool& flag_value) noexcept
        : gate{gate_value},
          changed{changed_value},
          flag{flag_value} {}

    ~release_flag_on_exit() noexcept { release_flag(gate, changed, flag); }

    release_flag_on_exit(const release_flag_on_exit&) = delete;
    release_flag_on_exit& operator=(const release_flag_on_exit&) = delete;
    release_flag_on_exit(release_flag_on_exit&&) = delete;
    release_flag_on_exit& operator=(release_flag_on_exit&&) = delete;
};

std::thread fail_after_worker_gate(void* const context,
    [[maybe_unused]] worker_entry entry,
    [[maybe_unused]] void* worker_context) {
    auto& state = *static_cast<session_factory_state*>(context);
    {
        std::unique_lock lock{state.gate};
        state.worker_launcher_entered = true;
        state.changed.notify_all();
        state.changed.wait(lock, [&state] { return state.worker_launcher_released; });
    }
    throw std::runtime_error{"injected maintenance worker launch failure"};
}

[[nodiscard]] std::unique_ptr<native_session> prepare_session(void* const context) {
    auto& state = *static_cast<session_factory_state*>(context);
    const auto attempt = ++state.attempts;
    if (state.owner != nullptr && state.reenter_owner.exchange(false, std::memory_order_acq_rel)) {
        state.reentry_returned_null.store(state.owner->bootstrap() == nullptr,
            std::memory_order_release);
    }
    if (attempt > 1U) {
        state.retry_after_rollback.store(state.rollback_completed.load(std::memory_order_acquire),
            std::memory_order_release);
    }

    bool block_factory{};
    {
        std::scoped_lock lock{state.gate};
        block_factory = state.block_factory_once;
        state.block_factory_once = false;
        if (block_factory) {
            state.factory_entered = true;
            state.changed.notify_all();
        }
    }
    if (block_factory) {
        std::unique_lock lock{state.gate};
        state.changed.wait(lock, [&state] { return state.factory_released; });
    }

    native_session_dependencies options{};
    options.audio_factory = inputnext::test::audio::make_unavailable_native_port_factory();
    options.audio_launcher = inputnext::audio_detail::make_std_audio_worker_launcher();
    options.identity = test_runtime_id();
    options.runtime_library = std::filesystem::path{L"C:\\fixture\\GameInputRedist.dll"};
    auto native = std::make_unique<fake_gameinput>();
    if (state.fail_native_start.exchange(false, std::memory_order_acq_rel)) {
        native->fail_next_watch_registration_with_partial_token();
    }
    state.native.store(native.get(), std::memory_order_release);
    options.native = std::move(native);
    options.metadata = std::make_unique<fake_metadata_provider>();
    options.focus_source = std::make_unique<inputnext::xinput_test_support::focus_source>();

    const bool fail_worker =
        state.fail_worker_launch_once.exchange(false, std::memory_order_acq_rel);
    if (fail_worker) {
        options.launch_worker = &fail_after_worker_gate;
        options.launch_context = &state;
    }

    try {
        return native_session::prepare(std::move(options));
    } catch (...) {
        if (fail_worker) {
            state.rollback_completed.store(true, std::memory_order_release);
        }
        throw;
    }
}

struct bootstrap_fixture final {
    session_factory_state factory;
    std::unique_ptr<controlled_focus_source> focus;
    controlled_focus_source* focus_view{};
    bootstrap_owner owner;

    bootstrap_fixture()
        : focus{std::make_unique<controlled_focus_source>()},
          focus_view{focus.get()},
          owner{std::move(focus), &prepare_session, &factory} {
        factory.owner = &owner;
    }
};

} // namespace

TEST_CASE("runtime.bootstrap.background_and_unavailable_focus_leave_the_graph_cold",
    "[integration][runtime][bootstrap]") {
    bootstrap_fixture fixture;
    fixture.focus_view->set(focus::evidence::background);
    CHECK(fixture.owner.bootstrap() == nullptr);
    CHECK(fixture.owner.state() == bootstrap_state::cold);
    CHECK(fixture.factory.attempts.load() == 0U);

    fixture.focus_view->set(focus::evidence::unavailable);
    CHECK(fixture.owner.bootstrap() == nullptr);
    CHECK(fixture.owner.state() == bootstrap_state::cold);
    CHECK(fixture.factory.attempts.load() == 0U);

    fixture.focus_view->set(focus::evidence::foreground);
    auto* const service = fixture.owner.bootstrap();
    REQUIRE(service != nullptr);
    CHECK(fixture.owner.state() == bootstrap_state::ready);
    CHECK(fixture.factory.attempts.load() == 1U);
}

TEST_CASE("runtime.bootstrap.one_foreground_caller_publishes_one_real_ready_session",
    "[integration][runtime][bootstrap][concurrency]") {
    bootstrap_fixture fixture;
    fixture.factory.reenter_owner.store(true, std::memory_order_release);
    {
        std::scoped_lock lock{fixture.factory.gate};
        fixture.factory.block_factory_once = true;
    }

    bool factory_entered{};
    xinput::service* first_result{};
    xinput::service* second_result{};
    std::jthread first;
    std::jthread second;
    const release_flag_on_exit release{fixture.factory.gate,
        fixture.factory.changed,
        fixture.factory.factory_released};

    first = std::jthread([&] { first_result = fixture.owner.bootstrap(); });
    factory_entered = wait_for_flag(fixture.factory.gate,
        fixture.factory.changed,
        fixture.factory.factory_entered,
        std::chrono::seconds{2});
    second = std::jthread([&] { second_result = fixture.owner.bootstrap(); });
    release_flag(fixture.factory.gate, fixture.factory.changed, fixture.factory.factory_released);
    first.join();
    second.join();

    CHECK(factory_entered);
    REQUIRE(first_result != nullptr);
    CHECK(second_result == first_result);
    CHECK(fixture.factory.attempts.load() == 1U);
    CHECK(fixture.factory.reentry_returned_null.load(std::memory_order_acquire));
    CHECK(fixture.owner.state() == bootstrap_state::ready);
    CHECK(fixture.owner.terminal_outcome().has_value());
    auto* const native = fixture.factory.native.load(std::memory_order_acquire);
    REQUIRE(native != nullptr);
    CHECK(native->initialized);

    const auto focus_observations = fixture.focus_view->observations();
    fixture.focus_view->set(focus::evidence::background);
    CHECK(fixture.owner.bootstrap() == first_result);
    CHECK(fixture.focus_view->observations() == focus_observations);
    CHECK(fixture.owner.presentation_if_published() != nullptr);
    CHECK(fixture.owner.presentation_if_published()->connection_status(0U) ==
          ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("runtime.bootstrap.preparation_rollback_finishes_before_retry_or_waiter_claim",
    "[integration][runtime][bootstrap][concurrency][failure]") {
    bootstrap_fixture fixture;
    fixture.factory.fail_worker_launch_once.store(true, std::memory_order_release);

    xinput::service* first_result{};
    xinput::service* second_result{};
    std::jthread first;
    std::jthread second;
    const release_flag_on_exit release{fixture.factory.gate,
        fixture.factory.changed,
        fixture.factory.worker_launcher_released};

    first = std::jthread([&] { first_result = fixture.owner.bootstrap(); });
    const bool rollback_blocked = wait_for_flag(fixture.factory.gate,
        fixture.factory.changed,
        fixture.factory.worker_launcher_entered,
        std::chrono::seconds{2});
    second = std::jthread([&] { second_result = fixture.owner.bootstrap(); });
    release_flag(fixture.factory.gate,
        fixture.factory.changed,
        fixture.factory.worker_launcher_released);
    first.join();
    second.join();

    CHECK(rollback_blocked);
    CHECK(first_result == nullptr);
    REQUIRE(second_result != nullptr);
    CHECK(fixture.factory.attempts.load() == 2U);
    CHECK(fixture.factory.rollback_completed.load(std::memory_order_acquire));
    CHECK(fixture.factory.retry_after_rollback.load(std::memory_order_acquire));
    CHECK(fixture.owner.state() == bootstrap_state::ready);
    CHECK(fixture.owner.terminal_outcome().has_value());
}

TEST_CASE("runtime.bootstrap.native_start_failure_is_terminal_degraded_with_published_service",
    "[integration][runtime][bootstrap][failure]") {
    bootstrap_fixture fixture;
    fixture.factory.fail_native_start.store(true, std::memory_order_release);

    auto* const service = fixture.owner.bootstrap();
    REQUIRE(service != nullptr);
    CHECK(fixture.owner.state() == bootstrap_state::degraded);
    CHECK_FALSE(fixture.owner.terminal_outcome().has_value());
    CHECK(fixture.factory.attempts.load() == 1U);

    const auto observations = fixture.focus_view->observations();
    fixture.focus_view->set(focus::evidence::background);
    CHECK(fixture.owner.bootstrap() == service);
    CHECK(fixture.owner.state() == bootstrap_state::degraded);
    CHECK(fixture.owner.terminal_outcome().error().code == gameinput_error::registration_failed);
    CHECK(fixture.factory.attempts.load() == 1U);
    CHECK(fixture.focus_view->observations() == observations);
    auto* const native = fixture.factory.native.load(std::memory_order_acquire);
    REQUIRE(native != nullptr);
    CHECK(native->initialized);
}
