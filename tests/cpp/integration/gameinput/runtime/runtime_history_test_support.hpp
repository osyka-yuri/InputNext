#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::gameinput_test_support {

using namespace inputnext::gameinput_detail;

struct native_history_script final {
    std::mutex gate;
    std::condition_variable changed;
    native_history_seed seed{
        .baseline =
            input_update{
                .state = gamepad_state{.buttons = 1U},
                .source_timestamp_us = 100U,
                .observation_qpc = 1'000U,
            },
        .qpc_frequency = 1'000'000U,
    };
    std::deque<native_history_step> steps;
    std::optional<native_failure> start_failure;
    std::uint64_t qpc_frequency{1'000'000U};
    std::uint64_t last_qpc{1'000U};
    std::atomic<std::size_t> cursor_factories;
    std::atomic<std::size_t> cursor_destructions;
    bool block_next_call{};
    bool next_call_entered{};
    bool release_next_call{};

    void enqueue(native_history_step step) {
        std::scoped_lock lock{gate};
        steps.push_back(step);
    }

    void block_next() {
        std::scoped_lock lock{gate};
        block_next_call = true;
        next_call_entered = false;
        release_next_call = false;
    }

    [[nodiscard]] bool wait_until_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock lock{gate};
        return changed.wait_for(lock, timeout, [this] { return next_call_entered; });
    }

    void release_blocked_call() {
        {
            std::scoped_lock lock{gate};
            release_next_call = true;
        }
        changed.notify_all();
    }
};

class scripted_native_history_cursor final : public native_history_cursor {
public:
    explicit scripted_native_history_cursor(std::shared_ptr<native_history_script> script) noexcept
        : script_(std::move(script)) {}

    ~scripted_native_history_cursor() override {
        script_->cursor_destructions.fetch_add(1U, std::memory_order_relaxed);
    }

    scripted_native_history_cursor(const scripted_native_history_cursor&) = delete;
    scripted_native_history_cursor& operator=(const scripted_native_history_cursor&) = delete;
    scripted_native_history_cursor(scripted_native_history_cursor&&) = delete;
    scripted_native_history_cursor& operator=(scripted_native_history_cursor&&) = delete;

    [[nodiscard]] native_result<native_history_seed> start() noexcept override {
        std::scoped_lock lock{script_->gate};
        if (script_->start_failure.has_value()) {
            const auto failure = *script_->start_failure;
            script_->start_failure.reset();
            return std::unexpected(failure);
        }
        script_->last_qpc = script_->seed.baseline.observation_qpc;
        return script_->seed;
    }

    [[nodiscard]] native_history_step next() noexcept override {
        std::unique_lock lock{script_->gate};
        if (script_->block_next_call) {
            script_->next_call_entered = true;
            script_->changed.notify_all();
            script_->changed.wait(lock, [this] { return script_->release_next_call; });
            script_->block_next_call = false;
            script_->release_next_call = false;
        }

        if (script_->steps.empty()) {
            return native_history_step{
                .kind = native_history_step_kind::caught_up,
                .checkpoint_qpc = script_->last_qpc,
            };
        }

        const auto step = script_->steps.front();
        script_->steps.pop_front();
        if (step.kind == native_history_step_kind::reading) {
            script_->last_qpc = step.reading.observation_qpc;
        }
        return step;
    }

private:
    std::shared_ptr<native_history_script> script_;
};

} // namespace inputnext::gameinput_test_support
