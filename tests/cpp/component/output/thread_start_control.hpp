#pragma once

#include "output/executor/thread_start_hook.hpp"

#include <chrono>

namespace inputnext::output::detail::thread_start_testing {

void fail_next_start(role value) noexcept;
void pause_next_start(role value) noexcept;
[[nodiscard]] bool wait_until_paused(std::chrono::milliseconds timeout);
void release_pause() noexcept;
void reset() noexcept;

} // namespace inputnext::output::detail::thread_start_testing
