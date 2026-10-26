#pragma once

#include <atomic>
#include <chrono>
#include <semaphore>

namespace inputnext::audio_detail {

class control_notification final {
public:
    void notify() noexcept {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) {
            signal_.release();
        }
    }

    [[nodiscard]] bool wait_for(const std::chrono::milliseconds timeout) {
        if (!signal_.try_acquire_for(timeout)) {
            return false;
        }
        // Acquire publications coalesced after the token was released.
        static_cast<void>(pending_.exchange(false, std::memory_order_acq_rel));
        return true;
    }

private:
    std::atomic<bool> pending_{false};
    std::binary_semaphore signal_{0};
};

} // namespace inputnext::audio_detail
