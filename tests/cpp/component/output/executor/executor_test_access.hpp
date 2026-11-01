#pragma once

#include "output/executor/executor_state.hpp"
#include "output/executor/thread_start_hook.hpp"
#include "thread_start_control.hpp"
#include <inputnext/output_executor.hpp>

#include <chrono>
#include <mutex>

namespace inputnext::output::detail {

struct executor_test_access final {
    static void fail_next_coordinator_start() noexcept {
        thread_start_testing::fail_next_start(thread_start_testing::role::coordinator);
    }

    static void fail_next_delivery_start() noexcept {
        thread_start_testing::fail_next_start(thread_start_testing::role::delivery_worker);
    }

    static void pause_next_delivery_start() noexcept {
        thread_start_testing::pause_next_start(thread_start_testing::role::delivery_worker);
    }

    [[nodiscard]] static bool wait_for_paused_delivery_start(
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        return thread_start_testing::wait_until_paused(timeout);
    }

    [[nodiscard]] static bool wait_for_engine_closed(executor& value,
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        auto& state = *value.implementation_;
        std::unique_lock lock{state.lifecycle_gate};
        return state.lifecycle_changed.wait_for(lock, timeout, [&state] {
            return state.engine_closed;
        });
    }

    static void release_paused_start() noexcept { thread_start_testing::release_pause(); }

    static void reset_start_control() noexcept { thread_start_testing::reset(); }
};

} // namespace inputnext::output::detail
