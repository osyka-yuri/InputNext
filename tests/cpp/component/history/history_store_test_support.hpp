#pragma once

#include "cursor.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace inputnext::history_detail {} // namespace inputnext::history_detail

namespace inputnext::history_component_test {

using namespace inputnext;
using namespace inputnext::history_detail;

inline constexpr auto test_now = history_time{};

[[nodiscard]] inline connection_target make_target(std::uint64_t value) noexcept {
    runtime_id runtime{};
    runtime.bytes[0U] = std::byte{0x51};
    return connection_target{
        .runtime = runtime,
        .device = device_key{.value = value},
        .connection = connection_id{.value = value},
    };
}

[[nodiscard]] inline history_sample
make_sample(std::uint64_t timestamp, std::uint64_t qpc, button_mask buttons = 1U) noexcept {
    return history_sample{
        .state = gamepad_state{.buttons = buttons},
        .source_timestamp_us = timestamp,
        .effective_qpc = qpc,
        .guide_known = true,
    };
}

struct fake_cursor_state final {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<history_failure> start_failures;
    std::deque<cursor_step> steps;
    history_sample baseline{make_sample(100U, 10U)};
    std::uint64_t qpc_frequency{1'000'000U};
    std::uint64_t last_qpc{10U};
    std::size_t start_calls{};
    bool block_next_call{};
    bool next_call_entered{};
    bool release_next_call{};
    bool block_destruction_call{};
    bool destruction_entered{};
    bool release_destruction_call{};
    std::atomic<std::size_t>* destruction_count{};

    void fail_start(history_error error) {
        std::scoped_lock lock{mutex};
        start_failures.push_back(history_failure{.code = error});
    }

    void enqueue(cursor_step step) {
        std::scoped_lock lock{mutex};
        steps.push_back(step);
    }

    void enqueue_reading(history_sample sample,
        history_record_origin origin = history_record_origin::native_pad) {
        enqueue(
            cursor_step{.kind = cursor_step_kind::reading, .reading = sample, .origin = origin});
    }

    void enqueue_provider_failure(std::uint32_t native_code = 0U) {
        enqueue(
            cursor_step{.kind = cursor_step_kind::provider_failure, .native_code = native_code});
    }

    void enqueue_history_lost(history_loss loss,
        std::uint64_t lost_count,
        std::uint32_t native_code = 0U) {
        enqueue(cursor_step{.kind = cursor_step_kind::history_lost,
            .loss = loss,
            .lost_count = lost_count,
            .native_code = native_code});
    }

    void block_next() {
        std::scoped_lock lock{mutex};
        block_next_call = true;
        next_call_entered = false;
        release_next_call = false;
    }

    [[nodiscard]] bool wait_until_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, timeout, [this] { return next_call_entered; });
    }

    void release_blocked_call() {
        {
            std::scoped_lock lock{mutex};
            release_next_call = true;
        }
        changed.notify_all();
    }

    void block_destruction() {
        std::scoped_lock lock{mutex};
        block_destruction_call = true;
        destruction_entered = false;
        release_destruction_call = false;
    }

    [[nodiscard]] bool wait_until_destroying(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, timeout, [this] { return destruction_entered; });
    }

    void release_destruction() {
        {
            std::scoped_lock lock{mutex};
            release_destruction_call = true;
        }
        changed.notify_all();
    }

    void pause_before_destruction() {
        std::unique_lock lock{mutex};
        if (!block_destruction_call) {
            return;
        }
        destruction_entered = true;
        changed.notify_all();
        changed.wait(lock, [this] { return release_destruction_call; });
        block_destruction_call = false;
        release_destruction_call = false;
    }
};

class fake_history_cursor final : public history_cursor {
public:
    explicit fake_history_cursor(std::shared_ptr<fake_cursor_state> state) noexcept
        : state_(std::move(state)) {}

    fake_history_cursor(const fake_history_cursor&) = delete;
    fake_history_cursor& operator=(const fake_history_cursor&) = delete;
    fake_history_cursor(fake_history_cursor&&) = delete;
    fake_history_cursor& operator=(fake_history_cursor&&) = delete;

    ~fake_history_cursor() override {
        state_->pause_before_destruction();
        state_->destruction_count->fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] history_result<cursor_seed> start(
        [[maybe_unused]] history_time now) noexcept override {
        std::scoped_lock lock{state_->mutex};
        ++state_->start_calls;
        if (!state_->start_failures.empty()) {
            const auto error = state_->start_failures.front();
            state_->start_failures.pop_front();
            return std::unexpected(error);
        }

        state_->last_qpc = state_->baseline.effective_qpc;
        return cursor_seed{
            .baseline = state_->baseline,
            .qpc_frequency = state_->qpc_frequency,
            .source = history_source_kind::native_history,
        };
    }

    [[nodiscard]] cursor_step next([[maybe_unused]] history_time now) noexcept override {
        std::unique_lock lock{state_->mutex};
        if (state_->block_next_call) {
            state_->next_call_entered = true;
            state_->changed.notify_all();
            state_->changed.wait(lock, [this] { return state_->release_next_call; });
            state_->block_next_call = false;
            state_->release_next_call = false;
        }

        if (state_->steps.empty()) {
            return cursor_step{
                .kind = cursor_step_kind::caught_up,
                .checkpoint_qpc = state_->last_qpc,
            };
        }

        const auto step = state_->steps.front();
        state_->steps.pop_front();
        if (step.kind == cursor_step_kind::reading) {
            state_->last_qpc = step.reading.effective_qpc;
        }
        return step;
    }

private:
    std::shared_ptr<fake_cursor_state> state_;
};

class fake_history_provider final : public history_provider {
public:
    fake_history_provider() : cursor_destructions{0U} {}

    [[nodiscard]] history_result<std::unique_ptr<history_cursor>> open_cursor(
        connection_target target) override {
        auto state = std::shared_ptr<fake_cursor_state>{};
        {
            std::scoped_lock lock{mutex_};
            if (!is_live_locked(target)) {
                return std::unexpected(history_failure{.code = history_error::disconnected});
            }
            if (!factory_failures_.empty()) {
                const auto error = factory_failures_.front();
                factory_failures_.pop_front();
                return std::unexpected(error);
            }

            state = std::make_shared<fake_cursor_state>();
            state->destruction_count = &cursor_destructions;
            cursors_[target.connection.value] = state;
            factory_calls.fetch_add(1U, std::memory_order_relaxed);
        }

        auto cursor = std::unique_ptr<history_cursor>{new fake_history_cursor{state}};
        wait_for_factory_release();
        return std::move(cursor);
    }

    [[nodiscard]] bool is_live(connection_target target) noexcept override {
        std::scoped_lock lock{mutex_};
        return is_live_locked(target);
    }

    void fail_next_factory(history_error error) {
        std::scoped_lock lock{mutex_};
        factory_failures_.push_back(history_failure{.code = error});
    }

    void block_next_factory_return() {
        std::scoped_lock lock{factory_mutex_};
        block_next_factory_ = true;
        factory_entered_ = false;
        release_factory_ = false;
    }

    [[nodiscard]] bool wait_until_factory_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock lock{factory_mutex_};
        return factory_changed_.wait_for(lock, timeout, [this] { return factory_entered_; });
    }

    void release_blocked_factory() {
        {
            std::scoped_lock lock{factory_mutex_};
            release_factory_ = true;
        }
        factory_changed_.notify_all();
    }

    void set_live(connection_target target, bool live) {
        std::scoped_lock lock{mutex_};
        live_[target.connection.value] = live;
    }

    [[nodiscard]] std::shared_ptr<fake_cursor_state> cursor_state(connection_target target) const {
        std::scoped_lock lock{mutex_};
        const auto found = cursors_.find(target.connection.value);
        return found == cursors_.end() ? nullptr : found->second;
    }

    std::atomic<std::size_t> cursor_destructions;
    std::atomic<std::size_t> factory_calls;

private:
    void wait_for_factory_release() {
        std::unique_lock lock{factory_mutex_};
        if (!block_next_factory_) {
            return;
        }
        block_next_factory_ = false;
        factory_entered_ = true;
        factory_changed_.notify_all();
        factory_changed_.wait(lock, [this] { return release_factory_; });
        release_factory_ = false;
    }

    [[nodiscard]] bool is_live_locked(connection_target target) const noexcept {
        const auto found = live_.find(target.connection.value);
        return found == live_.end() || found->second;
    }

    mutable std::mutex mutex_;
    std::deque<history_failure> factory_failures_;
    std::unordered_map<std::uint64_t, std::shared_ptr<fake_cursor_state>> cursors_;
    std::unordered_map<std::uint64_t, bool> live_;
    std::mutex factory_mutex_;
    std::condition_variable factory_changed_;
    bool block_next_factory_{};
    bool factory_entered_{};
    bool release_factory_{};
};

} // namespace inputnext::history_component_test
