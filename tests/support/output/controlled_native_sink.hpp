#pragma once

#include <inputnext/native_output.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

namespace inputnext::test::output {

class controlled_native_state final {
public:
    static constexpr std::size_t call_capacity = 16U;

    void set_next_result(inputnext::output::native_result result) noexcept {
        std::scoped_lock lock{mutex_};
        next_result_ = result;
    }

    void block_next_write() noexcept {
        std::scoped_lock lock{mutex_};
        blocked_write_ = call_count_ + 1U;
        released_write_ = 0U;
    }

    void release_write() noexcept {
        {
            std::scoped_lock lock{mutex_};
            released_write_ = blocked_write_ > call_count_ ? blocked_write_ : call_count_;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_write_count(std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] { return call_count_ >= count; });
    }

    [[nodiscard]] bool wait_for_blocked_write(
        std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return blocked_entered_ != 0U; });
    }

    [[nodiscard]] std::size_t call_count() const noexcept {
        std::scoped_lock lock{mutex_};
        return call_count_;
    }

    [[nodiscard]] inputnext::output::native_command call(std::size_t index) const noexcept {
        std::scoped_lock lock{mutex_};
        return index < call_count_ && index < calls_.size() ? calls_[index]
                                                            : inputnext::output::native_command{};
    }

    void block_destruction() noexcept {
        std::scoped_lock lock{mutex_};
        block_destruction_ = true;
        release_destruction_ = false;
    }

    void release_destruction() noexcept {
        {
            std::scoped_lock lock{mutex_};
            release_destruction_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_destruction(
        std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return destructions_ > 0U; });
    }

    [[nodiscard]] std::size_t destruction_count() const noexcept {
        std::scoped_lock lock{mutex_};
        return destructions_;
    }

private:
    friend class controlled_native_sink;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::array<inputnext::output::native_command, call_capacity> calls_{};
    std::size_t call_count_{};
    std::size_t blocked_write_{};
    std::size_t blocked_entered_{};
    std::size_t released_write_{};
    std::size_t destructions_{};
    bool block_destruction_{};
    bool release_destruction_{true};
    inputnext::output::native_result next_result_{
        .status = inputnext::output::native_status::accepted};
};

class controlled_native_sink final : public inputnext::output::native_sink {
public:
    explicit controlled_native_sink(std::shared_ptr<controlled_native_state> state) noexcept
        : state_(std::move(state)) {}

    controlled_native_sink(const controlled_native_sink&) = delete;
    controlled_native_sink& operator=(const controlled_native_sink&) = delete;
    controlled_native_sink(controlled_native_sink&&) = delete;
    controlled_native_sink& operator=(controlled_native_sink&&) = delete;

    ~controlled_native_sink() noexcept override {
        auto& state = *state_;
        std::unique_lock lock{state.mutex_};
        ++state.destructions_;
        state.changed_.notify_all();
        state.changed_.wait(lock,
            [&state] { return !state.block_destruction_ || state.release_destruction_; });
    }

    [[nodiscard]] inputnext::output::native_result write(
        const inputnext::output::native_command& command) noexcept override {
        auto& state = *state_;
        std::unique_lock lock{state.mutex_};
        if (state.call_count_ < state.calls_.size()) {
            state.calls_[state.call_count_] = command;
        }
        ++state.call_count_;
        const auto call_index = state.call_count_;
        const auto result = state.next_result_;
        state.next_result_ =
            inputnext::output::native_result{.status = inputnext::output::native_status::accepted};
        state.changed_.notify_all();
        if (state.blocked_write_ == call_index) {
            state.blocked_entered_ = call_index;
            state.changed_.notify_all();
            state.changed_.wait(lock,
                [&state, call_index] { return state.released_write_ >= call_index; });
            state.blocked_write_ = 0U;
            state.blocked_entered_ = 0U;
        }
        return result;
    }

private:
    std::shared_ptr<controlled_native_state> state_;
};

} // namespace inputnext::test::output
