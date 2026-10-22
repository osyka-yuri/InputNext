#pragma once

#include "executor_test_access.hpp"
#include "output/controlled_native_sink.hpp"
#include <inputnext/output_executor.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace inputnext::test::output {

[[nodiscard]] inline std::unique_ptr<inputnext::output::executor> start_executor(
    inputnext::output::authority& owner) {
    auto created = inputnext::output::executor::create(owner);
    if (!created.has_value()) {
        throw std::runtime_error{"executor creation failed"};
    }
    return std::move(*created);
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
    const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

class release_native_controls final {
public:
    explicit release_native_controls(std::shared_ptr<controlled_native_state> state) noexcept
        : state_(std::move(state)) {}

    ~release_native_controls() noexcept {
        state_->release_write();
        state_->release_destruction();
    }

    release_native_controls(const release_native_controls&) = delete;
    release_native_controls& operator=(const release_native_controls&) = delete;
    release_native_controls(release_native_controls&&) = delete;
    release_native_controls& operator=(release_native_controls&&) = delete;

private:
    std::shared_ptr<controlled_native_state> state_;
};

class reset_thread_start_control final {
public:
    ~reset_thread_start_control() noexcept {
        inputnext::output::detail::executor_test_access::reset_start_control();
    }

    reset_thread_start_control() = default;
    reset_thread_start_control(const reset_thread_start_control&) = delete;
    reset_thread_start_control& operator=(const reset_thread_start_control&) = delete;
    reset_thread_start_control(reset_thread_start_control&&) = delete;
    reset_thread_start_control& operator=(reset_thread_start_control&&) = delete;
};

} // namespace inputnext::test::output
