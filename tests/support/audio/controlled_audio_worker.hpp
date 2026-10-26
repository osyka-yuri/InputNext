#pragma once

#include "audio/audio_service.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace inputnext::test::audio {

inline constexpr std::size_t controlled_audio_worker_capacity = 32U;

class controlled_audio_worker_state final {
public:
    controlled_audio_worker_state() noexcept {
        for (auto& worker : workers_) {
            worker.join_allowed = true;
        }
    }

    void block_join_for(const std::size_t index) noexcept {
        std::scoped_lock lock{mutex_};
        if (index < workers_.size()) {
            workers_[index].join_allowed = false;
        }
    }

    void allow_join_for(const std::size_t index) noexcept {
        std::scoped_lock lock{mutex_};
        if (index < workers_.size()) {
            workers_[index].join_allowed = true;
        }
    }

    void allow_all_joins() noexcept {
        std::scoped_lock lock{mutex_};
        for (auto& worker : workers_) {
            worker.join_allowed = true;
        }
    }

    [[nodiscard]] std::size_t join_attempts(const std::size_t index) const noexcept {
        std::scoped_lock lock{mutex_};
        return index < workers_.size() ? workers_[index].join_attempts : 0U;
    }

    [[nodiscard]] std::size_t created_workers() const noexcept {
        std::scoped_lock lock{mutex_};
        return created_workers_;
    }

    [[nodiscard]] std::size_t register_worker() noexcept {
        std::scoped_lock lock{mutex_};
        if (created_workers_ == workers_.size()) {
            return workers_.size();
        }
        return created_workers_++;
    }

    [[nodiscard]] bool join_permitted(const std::size_t index) noexcept {
        std::scoped_lock lock{mutex_};
        if (index >= workers_.size()) {
            return false;
        }
        ++workers_[index].join_attempts;
        return workers_[index].join_allowed;
    }

private:
    struct worker_record final {
        std::size_t join_attempts{};
        bool join_allowed{};
    };

    mutable std::mutex mutex_;
    std::array<worker_record, controlled_audio_worker_capacity> workers_{};
    std::size_t created_workers_{};
};

class controlled_audio_worker final : public audio_detail::audio_worker {
public:
    controlled_audio_worker(std::shared_ptr<controlled_audio_worker_state> state,
        const std::size_t index,
        const audio_detail::audio_worker_launcher::entry_point entry,
        void* const context)
        : state_(std::move(state)),
          index_(index),
          thread_([entry, context] { entry(context); }) {}

    [[nodiscard]] bool joinable() const noexcept override { return thread_.joinable(); }

    [[nodiscard]] bool join() noexcept override {
        if (!thread_.joinable()) {
            return true;
        }
        if (!state_->join_permitted(index_)) {
            return false;
        }
        try {
            thread_.join();
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::shared_ptr<controlled_audio_worker_state> state_;
    std::size_t index_{};
    std::thread thread_;
};

class controlled_audio_worker_launcher final : public audio_detail::audio_worker_launcher {
public:
    explicit controlled_audio_worker_launcher(
        std::shared_ptr<controlled_audio_worker_state> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] std::unique_ptr<audio_detail::audio_worker> launch(const entry_point entry,
        void* const context) noexcept override {
        if (entry == nullptr || context == nullptr) {
            return {};
        }
        const auto index = state_->register_worker();
        if (index == controlled_audio_worker_capacity) {
            return {};
        }
        try {
            return std::make_unique<controlled_audio_worker>(state_, index, entry, context);
        } catch (...) {
            return {};
        }
    }

private:
    std::shared_ptr<controlled_audio_worker_state> state_;
};

} // namespace inputnext::test::audio
