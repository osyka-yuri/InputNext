#include "thread_start_control.hpp"

#include "output/executor/thread_start_hook.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>

namespace inputnext::output::detail::thread_start_testing {
namespace {

struct control final {
    std::mutex gate;
    std::condition_variable changed;
    role fail_next{role::none};
    role pause_next{role::none};
    bool pause_entered{};
    bool pause_released{};
};

control shared{};

} // namespace

void fail_next_start(const role value) noexcept {
    std::scoped_lock lock{shared.gate};
    shared.fail_next = value;
}

void pause_next_start(const role value) noexcept {
    std::scoped_lock lock{shared.gate};
    shared.pause_next = value;
    shared.pause_entered = false;
    shared.pause_released = false;
}

bool wait_until_paused(const std::chrono::milliseconds timeout) {
    std::unique_lock lock{shared.gate};
    return shared.changed.wait_for(lock, timeout, [] { return shared.pause_entered; });
}

void release_pause() noexcept {
    {
        std::scoped_lock lock{shared.gate};
        shared.pause_released = true;
    }
    shared.changed.notify_all();
}

void reset() noexcept {
    {
        std::scoped_lock lock{shared.gate};
        shared.fail_next = role::none;
        shared.pause_next = role::none;
        shared.pause_released = true;
    }
    shared.changed.notify_all();
}

void before_start(const role value) {
    std::unique_lock lock{shared.gate};
    if (shared.fail_next == value) {
        shared.fail_next = role::none;
        throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again)};
    }
    if (shared.pause_next != value) {
        return;
    }
    shared.pause_entered = true;
    shared.changed.notify_all();
    shared.changed.wait(lock, [] { return shared.pause_released; });
    shared.pause_next = role::none;
    shared.pause_entered = false;
    shared.pause_released = false;
}

} // namespace inputnext::output::detail::thread_start_testing
