#pragma once

#include <inputnext/focus.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace inputnext::test::focus {

class controlled_source final : public inputnext::focus::source {
public:
    controlled_source(inputnext::focus::process_instance process,
        std::vector<inputnext::focus::reading> readings,
        std::uint64_t blocked_call = 0U,
        std::atomic<std::size_t>* destruction_count = nullptr)
        : process_(process),
          readings_(std::move(readings)),
          blocked_call_(blocked_call),
          destruction_count_(destruction_count) {
        if (readings_.empty()) {
            throw std::invalid_argument{"controlled focus source requires at least one reading"};
        }
    }

    ~controlled_source() noexcept override {
        if (destruction_count_ != nullptr) {
            destruction_count_->fetch_add(1U, std::memory_order_relaxed);
        }
    }

    controlled_source(const controlled_source&) = delete;
    controlled_source& operator=(const controlled_source&) = delete;
    controlled_source(controlled_source&&) = delete;
    controlled_source& operator=(controlled_source&&) = delete;

    [[nodiscard]] inputnext::focus::process_instance process() const noexcept override {
        return process_;
    }

    [[nodiscard]] inputnext::focus::reading observe() noexcept override {
        std::unique_lock lock{mutex_};
        const auto call = ++calls_;
        changed_.notify_all();
        if (call == blocked_call_) {
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        } else if (call == next_blocked_call_) {
            next_blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return next_released_; });
        }

        const auto index =
            std::min<std::size_t>(static_cast<std::size_t>(call - 1U), readings_.size() - 1U);
        const auto result = readings_[index];
        ++returned_;
        changed_.notify_all();
        return result;
    }

    [[nodiscard]] bool wait_until_blocked(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return blocked_; });
    }

    [[nodiscard]] bool block_next_observation() noexcept {
        std::scoped_lock lock{mutex_};
        if (!blocked_ || released_ || next_blocked_call_ != 0U ||
            calls_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        next_blocked_call_ = calls_ + 1U;
        next_released_ = false;
        return true;
    }

    [[nodiscard]] bool wait_until_next_observation_blocked(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return next_blocked_; });
    }

    void release_initial_observation() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_returned(std::uint64_t count,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] { return returned_ >= count; });
    }

    void release() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        next_released_ = true;
        changed_.notify_all();
    }

private:
    const inputnext::focus::process_instance process_;
    const std::vector<inputnext::focus::reading> readings_;
    const std::uint64_t blocked_call_{};
    std::atomic<std::size_t>* const destruction_count_{};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t calls_{};
    std::uint64_t returned_{};
    std::uint64_t next_blocked_call_{};
    bool blocked_{};
    bool released_{};
    bool next_blocked_{};
    bool next_released_{};
};

} // namespace inputnext::test::focus
