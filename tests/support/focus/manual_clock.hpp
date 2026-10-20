#pragma once

#include <atomic>
#include <chrono>

namespace inputnext::test::focus {

class manual_clock final {
public:
    using clock = std::chrono::steady_clock;
    using duration = clock::duration;
    using time_point = clock::time_point;
    using rep = duration::rep;

    explicit manual_clock(time_point initial = {}) noexcept
        : ticks_(initial.time_since_epoch().count()) {}

    manual_clock(const manual_clock&) = delete;
    manual_clock& operator=(const manual_clock&) = delete;
    manual_clock(manual_clock&&) = delete;
    manual_clock& operator=(manual_clock&&) = delete;
    ~manual_clock() = default;

    [[nodiscard]] time_point now() const noexcept {
        return time_point{duration{ticks_.load(std::memory_order_relaxed)}};
    }

    void advance(duration amount) noexcept {
        ticks_.fetch_add(amount.count(), std::memory_order_relaxed);
    }

    [[nodiscard]] static time_point read(const void* context) noexcept {
        return static_cast<const manual_clock*>(context)->now();
    }

private:
    std::atomic<rep> ticks_;
};

} // namespace inputnext::test::focus
