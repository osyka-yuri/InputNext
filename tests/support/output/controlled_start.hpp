#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace inputnext::test::output {

class controlled_start final {
public:
    [[nodiscard]] bool arrive_and_wait() {
        std::unique_lock lock{mutex_};
        ++arrived_;
        changed_.notify_all();
        return changed_.wait_for(lock, std::chrono::seconds{2}, [this] { return released_; });
    }

    [[nodiscard]] bool wait_until_ready(std::size_t participants) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, std::chrono::seconds{2}, [this, participants] {
            return arrived_ >= participants;
        });
    }

    void release() noexcept {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t arrived_{};
    bool released_{};
};

template <typename... Threads>
void release_and_join(controlled_start& start, Threads&... threads) {
    start.release();
    (threads.join(), ...);
}

} // namespace inputnext::test::output
