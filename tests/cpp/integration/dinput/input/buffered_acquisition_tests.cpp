#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/buffered/service.hpp"
#include "dinput/buffered/service_test_access.hpp"
#include "dinput/input_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
#include <new>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

class lane_hook_context final {
public:
    [[nodiscard]] dinput::detail::buffered_input_test_hooks hooks() noexcept {
        return {.before_allocation = &before_allocation,
            .before_close = &before_close,
            .context = this};
    }

    void pause_next_allocation(const bool fail_after_release = false) noexcept {
        std::scoped_lock lock{gate_};
        allocation_pause_call_ = allocation_calls_ + 1U;
        allocation_released_ = false;
        fail_after_allocation_pause_ = fail_after_release;
    }

    void pause_next_close() noexcept {
        std::scoped_lock lock{gate_};
        close_pause_call_ = close_calls_ + 1U;
        close_released_ = false;
    }

    [[nodiscard]] bool wait_for_allocation_pause(const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this] { return allocation_paused_; });
    }

    [[nodiscard]] bool wait_for_allocation_calls(std::size_t expected,
        const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this, expected] {
            return allocation_calls_ >= expected;
        });
    }

    [[nodiscard]] bool wait_for_close_pause(const std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock{gate_};
        return changed_.wait_for(lock, timeout, [this] { return close_paused_; });
    }

    void release_all() noexcept {
        {
            std::scoped_lock lock{gate_};
            allocation_released_ = true;
            close_released_ = true;
        }
        changed_.notify_all();
    }

private:
    static void before_allocation(void* const context) {
        static_cast<lane_hook_context*>(context)->on_allocation();
    }

    static void before_close(void* const context) noexcept {
        static_cast<lane_hook_context*>(context)->on_close();
    }

    void on_allocation() {
        std::unique_lock lock{gate_};
        const auto call = ++allocation_calls_;
        changed_.notify_all();
        if (call != allocation_pause_call_) {
            return;
        }

        allocation_paused_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return allocation_released_; });
        allocation_paused_ = false;
        if (fail_after_allocation_pause_) {
            lock.unlock();
            throw std::bad_alloc{};
        }
    }

    void on_close() noexcept {
        std::unique_lock lock{gate_};
        const auto call = ++close_calls_;
        changed_.notify_all();
        if (call != close_pause_call_) {
            return;
        }

        close_paused_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return close_released_; });
        close_paused_ = false;
    }

    std::mutex gate_;
    std::condition_variable changed_;
    std::size_t allocation_calls_{};
    std::size_t allocation_pause_call_{};
    std::size_t close_calls_{};
    std::size_t close_pause_call_{};
    bool allocation_paused_{};
    bool allocation_released_{true};
    bool fail_after_allocation_pause_{};
    bool close_paused_{};
    bool close_released_{true};
};

class clear_hooks_on_exit final {
public:
    clear_hooks_on_exit(dinput::buffered_input& input, lane_hook_context& context) noexcept
        : input_(input),
          context_(context) {}

    ~clear_hooks_on_exit() {
        context_.release_all();
        dinput::detail::buffered_input_test_access::set_hooks(input_, {});
    }

    clear_hooks_on_exit(const clear_hooks_on_exit&) = delete;
    clear_hooks_on_exit& operator=(const clear_hooks_on_exit&) = delete;
    clear_hooks_on_exit(clear_hooks_on_exit&&) = delete;
    clear_hooks_on_exit& operator=(clear_hooks_on_exit&&) = delete;

private:
    dinput::buffered_input& input_;
    lane_hook_context& context_;
};

} // namespace

TEST_CASE("dinput.buffered_lane_creator_handoff_preserves_peer_on_allocation_failure",
    "[integration][dinput][input][buffered][acquisition][concurrency]") {
    for (const bool creator_fails : {false, true}) {
        core_test_support::fixture owners;
        const auto target = owners.connect(0U);
        prepare_buffered_history(owners);

        lane_hook_context hooks;
        hooks.pause_next_allocation(creator_fails);
        const auto access = make_access(owners, *owners.focus_observer);
        parent_unknown parent;
        const hidden_window window;
        auto creator = create_device(target, access, parent, 0x91U);
        auto peer = create_device(target, access, parent, 0x92U);
        auto third = create_device(target, access, parent, 0x93U);
        configure_compact_device(*creator.get(), window.get());
        configure_compact_device(*peer.get(), window.get());
        configure_compact_device(*third.get(), window.get());
        REQUIRE(set_buffer_size(*creator.get(), 8U) == DI_OK);
        REQUIRE(set_buffer_size(*peer.get(), 8U) == DI_OK);
        REQUIRE(set_buffer_size(*third.get(), 8U) == DI_OK);

        const auto call = access->enter();
        REQUIRE(static_cast<bool>(call));
        dinput::detail::buffered_input_test_access::set_hooks(call.buffers(), hooks.hooks());

        std::future<HRESULT> creator_acquire;
        std::future<HRESULT> peer_acquire;
        const clear_hooks_on_exit clear_hooks{call.buffers(), hooks};
        creator_acquire = std::async(std::launch::async, [&] { return creator->Acquire(); });
        REQUIRE(hooks.wait_for_allocation_pause());

        peer_acquire = std::async(std::launch::async, [&] { return peer->Acquire(); });
        const auto peer_while_creator_paused = peer_acquire.wait_for(2s);
        CHECK(peer_while_creator_paused == std::future_status::ready);

        hooks.release_all();
        const auto creator_status = creator_acquire.wait_for(2s);
        const auto peer_status = peer_acquire.wait_for(2s);
        CHECK(creator_status == std::future_status::ready);
        CHECK(peer_status == std::future_status::ready);
        if (creator_status == std::future_status::ready) {
            CHECK(creator_acquire.get() == (creator_fails ? DIERR_OUTOFMEMORY : DI_OK));
        }
        if (peer_status == std::future_status::ready) {
            CHECK(peer_acquire.get() == DI_OK);
        }

        compact_state state{};
        CHECK(peer->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
        if (creator_fails) {
            CHECK(third->Acquire() == DI_OK);
            CHECK(peer->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
            CHECK(third->Unacquire() == DI_OK);
        } else {
            CHECK(creator->Unacquire() == DI_OK);
        }
        CHECK(peer->Unacquire() == DI_OK);
    }
}

TEST_CASE("dinput.buffered_reacquire_joins_a_lane_while_its_reader_is_closing",
    "[integration][dinput][input][buffered][acquisition][concurrency]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    lane_hook_context hooks;
    const auto access = make_access(owners, *owners.focus_observer);
    parent_unknown parent;
    const hidden_window window;
    auto device = create_device(target, access, parent, 0x94U);
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    const auto factories_before_close = script->cursor_factories.load(std::memory_order_relaxed);
    hooks.pause_next_close();
    const auto call = access->enter();
    REQUIRE(static_cast<bool>(call));
    dinput::detail::buffered_input_test_access::set_hooks(call.buffers(), hooks.hooks());

    std::future<HRESULT> unacquire;
    std::future<HRESULT> reacquire;
    const clear_hooks_on_exit clear_hooks{call.buffers(), hooks};
    unacquire = std::async(std::launch::async, [&] { return device->Unacquire(); });
    REQUIRE(hooks.wait_for_close_pause());

    reacquire = std::async(std::launch::async, [&] { return device->Acquire(); });
    REQUIRE(hooks.wait_for_allocation_calls(1U));
    const auto pending_status = reacquire.wait_for(100ms);
    CHECK(pending_status == std::future_status::timeout);
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == factories_before_close);

    hooks.release_all();
    const auto unacquire_status = unacquire.wait_for(2s);
    const auto reacquire_status = reacquire.wait_for(2s);
    CHECK(unacquire_status == std::future_status::ready);
    CHECK(reacquire_status == std::future_status::ready);
    if (unacquire_status == std::future_status::ready) {
        CHECK(unacquire.get() == DI_OK);
    }
    if (reacquire_status == std::future_status::ready) {
        CHECK(reacquire.get() == DI_OK);
    }
    // A new reader can reuse the shared bank's existing native cursor.
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == factories_before_close);
    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.unacquire_cancels_reacquisition_waiting_for_a_closing_lane",
    "[integration][dinput][input][buffered][acquisition][cancellation]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    lane_hook_context hooks;
    const auto access = make_access(owners, *owners.focus_observer);
    parent_unknown parent;
    const hidden_window window;
    auto device = create_device(target, access, parent, 0x95U);
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    const auto factories_before_close = script->cursor_factories.load(std::memory_order_relaxed);
    hooks.pause_next_close();
    const auto call = access->enter();
    REQUIRE(static_cast<bool>(call));
    dinput::detail::buffered_input_test_access::set_hooks(call.buffers(), hooks.hooks());

    std::future<HRESULT> unacquire;
    std::future<HRESULT> reacquire;
    const clear_hooks_on_exit clear_hooks{call.buffers(), hooks};
    unacquire = std::async(std::launch::async, [&] { return device->Unacquire(); });
    REQUIRE(hooks.wait_for_close_pause());

    reacquire = std::async(std::launch::async, [&] { return device->Acquire(); });
    REQUIRE(hooks.wait_for_allocation_calls(1U));
    CHECK(device->Unacquire() == DI_OK);

    const auto reacquire_status = reacquire.wait_for(500ms);
    hooks.release_all();
    const auto unacquire_status = unacquire.wait_for(2s);
    CHECK(unacquire_status == std::future_status::ready);
    CHECK(reacquire_status == std::future_status::ready);
    if (unacquire_status == std::future_status::ready) {
        CHECK(unacquire.get() == DI_OK);
    }
    if (reacquire_status == std::future_status::ready) {
        CHECK(reacquire.get() == DIERR_INPUTLOST);
    }
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == factories_before_close);
    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_NOTACQUIRED);
}

TEST_CASE("dinput.shutdown_cancels_a_closing_lane_handoff_without_reopening_it",
    "[integration][dinput][input][buffered][acquisition][shutdown]") {
    core_test_support::fixture owners;
    const auto target = owners.connect();
    prepare_buffered_history(owners);
    lane_hook_context hooks;
    const auto access = make_access(owners, *owners.focus_observer);
    parent_unknown parent;
    const hidden_window window;
    auto device = create_device(target, access, parent, 0x97U);
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);
    const auto script = owners.scripts[0U];
    const auto factories = script->cursor_factories.load(std::memory_order_relaxed);
    hooks.pause_next_close();
    dinput::buffered_input* buffers{};
    {
        const auto call = access->enter();
        REQUIRE(static_cast<bool>(call));
        buffers = &call.buffers();
        dinput::detail::buffered_input_test_access::set_hooks(*buffers, hooks.hooks());
    }
    std::future<HRESULT> reacquire;
    std::future<void> drain;
    const clear_hooks_on_exit clear_hooks{*buffers, hooks};
    REQUIRE(device->Unacquire() == DI_OK);
    REQUIRE(hooks.wait_for_close_pause());
    reacquire = std::async(std::launch::async, [&] { return device->Acquire(); });
    REQUIRE(hooks.wait_for_allocation_calls(1U));
    access->close_admission();
    const auto cancelled = reacquire.wait_for(500ms);
    CHECK(cancelled == std::future_status::ready);
    if (cancelled == std::future_status::ready) {
        CHECK(reacquire.get() == DIERR_INPUTLOST);
    }
    drain = std::async(std::launch::async, [&] { access->drain(); });
    CHECK(drain.wait_for(20ms) == std::future_status::timeout);
    hooks.release_all();
    CHECK(drain.wait_for(2s) == std::future_status::ready);
    drain.get();
    CHECK(script->cursor_factories.load(std::memory_order_relaxed) == factories);
    CHECK(device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.buffered_fast_acquire_unacquire_transitions_do_not_lose_a_healthy_target",
    "[integration][dinput][input][buffered][acquisition][concurrency]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    prepare_buffered_history(owners);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    auto device = create_device(target, access, parent, 0x96U);
    configure_compact_device(*device.get(), window.get());
    REQUIRE(set_buffer_size(*device.get(), 8U) == DI_OK);

    for (std::size_t iteration = 0U; iteration < 20U; ++iteration) {
        REQUIRE(device->Acquire() == DI_OK);
        REQUIRE(device->Unacquire() == DI_OK);
        REQUIRE(device->Acquire() == DI_OK);
        compact_state state{};
        CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
        REQUIRE(device->Unacquire() == DI_OK);
        // The shared owner reclaims retired history banks between transition pairs.
        owners.gameinput.maintain();
    }
}
