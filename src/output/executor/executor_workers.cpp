#include "executor_state.hpp"

#include "output/authority/execution.hpp"
#include "output/execution/engine.hpp"
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <cstddef>
#include <expected>
#include <mutex>
#include <stop_token>
#include <thread>

#ifdef INPUTNEXT_OUTPUT_TESTING
#    include "thread_start_hook.hpp"
#endif

#include <exception>
#include <utility>

namespace inputnext::output {

execution_result<std::size_t> executor::implementation::reserve_worker_slot(
    const detail::lane_token lane) noexcept {
    std::scoped_lock lock{lifecycle_gate};
    if (!accepting) {
        return std::unexpected{execution_error::not_running};
    }
    for (std::size_t index = 0U; index < workers.size(); ++index) {
        auto& worker = workers[index];
        if (worker.phase == worker_phase::free) {
            worker.lane = lane;
            worker.phase = worker_phase::starting;
            return index;
        }
    }
    return std::unexpected{execution_error::capacity_exceeded};
}

void executor::implementation::release_unstarted_worker_slot(const std::size_t index,
    const detail::lane_token lane) noexcept {
    {
        std::scoped_lock lock{lifecycle_gate};
        auto& worker = workers[index];
        if (worker.lane == lane && worker.phase == worker_phase::starting) {
            worker = worker_record{};
        }
    }
    lifecycle_changed.notify_all();
}

execution_result<void> executor::implementation::start_delivery_worker(
    const detail::registration_reservation& reservation) {
    const auto slot = reserve_worker_slot(reservation.lane);
    if (!slot.has_value()) {
        const auto released = engine->worker_start_failed(reservation.lane);
        if (!released.has_value()) {
            // There is no thread to join, so the failed reservation cannot be finalized safely.
            std::terminate();
        }
        return std::unexpected{slot.error()};
    }

    std::jthread thread;
    try {
#ifdef INPUTNEXT_OUTPUT_TESTING
        detail::thread_start_testing::before_start(
            detail::thread_start_testing::role::delivery_worker);
#endif
        thread = std::jthread{[this, lane = reservation.lane](const std::stop_token& stop_token) {
            delivery_loop(lane, stop_token);
        }};
    } catch (...) {
        release_unstarted_worker_slot(*slot, reservation.lane);
        const auto released = engine->worker_start_failed(reservation.lane);
        if (!released.has_value()) {
            // Construction failed before a thread existed; worker_joined would be false evidence.
            std::terminate();
        }
        return std::unexpected{execution_error::resources};
    }

    bool installed = false;
    {
        std::scoped_lock lock{lifecycle_gate};
        auto& worker = workers[*slot];
        if (worker.lane == reservation.lane && worker.phase == worker_phase::starting) {
            worker.thread.swap(thread);
            worker.phase = worker_phase::running;
            installed = true;
        }
    }
    if (!installed) {
        release_unstarted_worker_slot(*slot, reservation.lane);
        // Admission closes regardless of rollback outcome; the join below proves quiescence.
        [[maybe_unused]] const auto canceled = engine->cancel_backend(reservation);
        close_admission();
        if (thread.joinable()) {
            thread.join();
        }
        const auto joined = engine->worker_joined(reservation.lane);
        if (!joined.has_value()) {
            std::terminate();
        }
        return std::unexpected{execution_error::resources};
    }

    const auto started = engine->worker_started(reservation.lane);
    if (started.has_value()) {
        return {};
    }

    const auto canceled = engine->cancel_backend(reservation);
    if (!canceled.has_value()) {
        close_admission();
    }
    if (engine->worker_should_exit(reservation.lane)) {
        const auto joined = stop_and_join_worker(reservation.lane);
        if (!joined.has_value()) {
            return std::unexpected{execution_error::resources};
        }
    }
    return std::unexpected{started.error()};
}

void executor::implementation::delivery_loop(const detail::lane_token lane,
    const std::stop_token& stop_token) noexcept {
    for (;;) {
        bool work_ready = false;
        try {
            work_ready = engine->wait_for_work(lane, stop_token);
        } catch (...) {
            close_admission();
            drain_after_wait_failure(lane);
            return;
        }

        if (!work_ready) {
            if (engine->worker_should_exit(lane)) {
                return;
            }
            close_admission();
            drain_after_wait_failure(lane);
            return;
        }
        if (!engine->drive_one(lane)) {
            std::this_thread::yield();
        }
    }
}

void executor::implementation::drain_after_wait_failure(
    const detail::lane_token lane) const noexcept {
    while (!engine->worker_should_exit(lane)) {
        if (!engine->drive_one(lane)) {
            std::this_thread::yield();
        }
    }
}

execution_result<void> executor::implementation::finish_worker_join(const std::size_t index,
    const detail::lane_token lane,
    std::jthread thread) noexcept {
    if (thread.joinable()) {
        thread.join();
    }
    const auto joined = engine->worker_joined(lane);
    {
        std::scoped_lock lock{lifecycle_gate};
        auto& worker = workers[index];
        if (worker.lane != lane || worker.phase != worker_phase::joining) {
            std::terminate();
        }
        if (joined.has_value()) {
            worker = worker_record{};
        } else {
            worker.phase = worker_phase::joined_pending;
        }
    }
    lifecycle_changed.notify_all();
    return joined;
}

execution_result<void> executor::implementation::stop_and_join_worker(
    const detail::lane_token lane) noexcept {
    if (!engine->worker_should_exit(lane)) {
        return {};
    }

    std::size_t index{};
    std::jthread thread;
    {
        std::unique_lock lock{lifecycle_gate};
        for (;;) {
            index = workers.size();
            for (std::size_t candidate = 0U; candidate < workers.size(); ++candidate) {
                if (workers[candidate].phase != worker_phase::free &&
                    workers[candidate].lane == lane) {
                    index = candidate;
                    break;
                }
            }
            if (index == workers.size()) {
                return {};
            }
            auto& worker = workers[index];
            if (worker.phase == worker_phase::joining || worker.phase == worker_phase::starting) {
                lifecycle_changed.wait(lock);
                continue;
            }
            worker.phase = worker_phase::joining;
            thread = std::move(worker.thread);
            break;
        }
    }
    return finish_worker_join(index, lane, std::move(thread));
}

execution_result<void> executor::implementation::join_worker_for_shutdown(
    const std::size_t worker_index) noexcept {
    detail::lane_token lane{};
    std::jthread thread;
    {
        std::scoped_lock lock{lifecycle_gate};
        auto& worker = workers[worker_index];
        if (worker.phase == worker_phase::free) {
            return {};
        }
        lane = worker.lane;
        if (worker.phase == worker_phase::starting || worker.phase == worker_phase::joining) {
            return std::unexpected{execution_error::busy};
        }
        worker.phase = worker_phase::joining;
        thread = std::move(worker.thread);
    }
    return finish_worker_join(worker_index, lane, std::move(thread));
}

void executor::implementation::shutdown() noexcept {
    {
        std::unique_lock lock{lifecycle_gate};
        if (shutdown_complete) {
            return;
        }
        if (shutdown_started) {
            lifecycle_changed.wait(lock, [this] { return shutdown_complete; });
            return;
        }
        shutdown_started = true;
        accepting = false;
    }

    engine->close_all();
    {
        std::scoped_lock lock{lifecycle_gate};
        engine_closed = true;
    }
    lifecycle_changed.notify_all();

    std::jthread coordinator_thread;
    {
        std::scoped_lock lock{lifecycle_gate};
        coordinator_thread = std::move(coordinator);
    }
    if (coordinator_thread.joinable()) {
        coordinator_thread.request_stop();
        coordinator_thread.join();
    }

    {
        std::unique_lock lock{lifecycle_gate};
        lifecycle_changed.wait(lock, [this] { return active_operations == 0U; });
    }

    for (std::size_t index = 0U; index < workers.size(); ++index) {
        const auto joined = join_worker_for_shutdown(index);
        if (!joined.has_value()) {
            std::terminate();
        }
    }

    engine.reset();
    const auto detached = detail::authority_execution_access::detach(owner, attached_owner);
    if (!detached.has_value()) {
        std::terminate();
    }

    {
        std::scoped_lock lock{lifecycle_gate};
        shutdown_complete = true;
    }
    lifecycle_changed.notify_all();
}

} // namespace inputnext::output
