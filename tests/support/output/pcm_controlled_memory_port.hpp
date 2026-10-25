#pragma once

#include "output/pcm/body_pcm_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext::test::output {

class pcm_controlled_memory_port final : public inputnext::output::detail::pcm_memory_port {
public:
    [[nodiscard]] float* allocate(const std::size_t sample_count) override {
        bool fail{};
        {
            std::unique_lock lock{mutex_};
            ++allocation_calls_;
            allocation_entered_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::seconds{2}, [this] {
                return !block_allocation_ || release_allocation_;
            });
            fail = fail_next_allocation_;
            fail_next_allocation_ = false;
        }
        return fail ? nullptr : new (std::nothrow) float[sample_count];
    }

    void deallocate(float* const samples) noexcept override {
        {
            std::unique_lock lock{mutex_};
            ++deallocation_calls_;
            deallocation_entered_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::seconds{2}, [this] {
                return !block_deallocation_ || release_deallocation_;
            });
        }
        delete[] samples;
    }

    void fail_next_allocation() noexcept {
        std::scoped_lock lock{mutex_};
        fail_next_allocation_ = true;
    }

    void block_next_allocation() noexcept {
        std::scoped_lock lock{mutex_};
        block_allocation_ = true;
        release_allocation_ = false;
        allocation_entered_ = false;
    }

    void block_next_deallocation() noexcept {
        std::scoped_lock lock{mutex_};
        block_deallocation_ = true;
        release_deallocation_ = false;
        deallocation_entered_ = false;
    }

    void release_all() noexcept {
        {
            std::scoped_lock lock{mutex_};
            release_allocation_ = true;
            release_deallocation_ = true;
            block_allocation_ = false;
            block_deallocation_ = false;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for_allocation(
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return allocation_entered_; });
    }

    [[nodiscard]] bool wait_for_deallocation(
        const std::chrono::milliseconds timeout = std::chrono::seconds{2}) const {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return deallocation_entered_; });
    }

    [[nodiscard]] std::size_t allocation_calls() const noexcept {
        std::scoped_lock lock{mutex_};
        return allocation_calls_;
    }

    [[nodiscard]] std::size_t deallocation_calls() const noexcept {
        std::scoped_lock lock{mutex_};
        return deallocation_calls_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t allocation_calls_{};
    std::size_t deallocation_calls_{};
    bool allocation_entered_{};
    bool deallocation_entered_{};
    bool fail_next_allocation_{};
    bool block_allocation_{};
    bool release_allocation_{true};
    bool block_deallocation_{};
    bool release_deallocation_{true};
};

class pcm_release_guard final {
public:
    explicit pcm_release_guard(std::shared_ptr<pcm_controlled_memory_port> port) noexcept
        : port_(std::move(port)) {}

    ~pcm_release_guard() noexcept { port_->release_all(); }

    pcm_release_guard(const pcm_release_guard&) = delete;
    pcm_release_guard& operator=(const pcm_release_guard&) = delete;
    pcm_release_guard(pcm_release_guard&&) = delete;
    pcm_release_guard& operator=(pcm_release_guard&&) = delete;

private:
    std::shared_ptr<pcm_controlled_memory_port> port_;
};

} // namespace inputnext::test::output
