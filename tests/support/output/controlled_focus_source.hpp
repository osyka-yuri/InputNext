#pragma once

#include <inputnext/focus.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace inputnext::test::output {

class controlled_focus_source final : public focus::source {
public:
    controlled_focus_source(focus::process_instance process, std::vector<focus::reading> readings)
        : process_(process),
          readings_(std::move(readings)) {
        if (readings_.empty()) {
            throw std::invalid_argument{"controlled focus source requires readings"};
        }
    }

    controlled_focus_source(const controlled_focus_source&) = delete;
    controlled_focus_source& operator=(const controlled_focus_source&) = delete;
    controlled_focus_source(controlled_focus_source&&) = delete;
    controlled_focus_source& operator=(controlled_focus_source&&) = delete;
    ~controlled_focus_source() override = default;

    [[nodiscard]] focus::process_instance process() const noexcept override { return process_; }

    [[nodiscard]] focus::reading observe() noexcept override {
        std::unique_lock lock{mutex_};
        ++calls_;
        changed_.notify_all();
        if (calls_ == blocked_call_) {
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        }
        const auto index =
            std::min<std::size_t>(static_cast<std::size_t>(calls_ - 1U), readings_.size() - 1U);
        return readings_[index];
    }

    void block_call(std::uint64_t call) noexcept {
        std::scoped_lock lock{mutex_};
        blocked_call_ = call;
        blocked_ = false;
        released_ = false;
    }

    [[nodiscard]] bool wait_until_blocked(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return blocked_; });
    }

    void release() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

private:
    const focus::process_instance process_;
    const std::vector<focus::reading> readings_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t calls_{};
    std::uint64_t blocked_call_{};
    bool blocked_{};
    bool released_{};
};

} // namespace inputnext::test::output
