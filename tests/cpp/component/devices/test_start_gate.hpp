#pragma once

#include <atomic>
#include <cstddef>

namespace inputnext::test::devices {

class start_gate final {
public:
    start_gate() = default;

    ~start_gate() { release(); }

    start_gate(const start_gate&) = delete;
    start_gate& operator=(const start_gate&) = delete;
    start_gate(start_gate&&) = delete;
    start_gate& operator=(start_gate&&) = delete;

    void arrive_and_wait() noexcept {
        ready_.fetch_add(1U, std::memory_order_relaxed);
        ready_.notify_all();

        auto is_released = released_.load(std::memory_order_acquire);
        while (!is_released) {
            released_.wait(false, std::memory_order_acquire);
            is_released = released_.load(std::memory_order_acquire);
        }
    }

    void wait_for_ready(std::size_t participant_count) noexcept {
        auto ready = ready_.load(std::memory_order_relaxed);
        while (ready < participant_count) {
            ready_.wait(ready, std::memory_order_relaxed);
            ready = ready_.load(std::memory_order_relaxed);
        }
    }

    void release() noexcept {
        released_.store(true, std::memory_order_release);
        released_.notify_all();
    }

private:
    std::atomic<std::size_t> ready_{0U};
    std::atomic<bool> released_{false};
};

} // namespace inputnext::test::devices
