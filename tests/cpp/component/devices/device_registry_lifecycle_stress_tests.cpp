#include <inputnext/device_registry.hpp>

#include "test_start_gate.hpp"
#include "test_support.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;
using namespace std::chrono_literals;

constexpr std::size_t reader_count = 3U;
constexpr std::size_t churn_rounds = 12U;
constexpr std::size_t generation_count = churn_rounds + 1U;

struct binding_view final {
    connection_target target{};
    read_binding reader{};
    producer_binding producer;
    std::size_t generation{};
};

struct current_connection final {
    std::mutex mutex;
    admitted_connection value{};
    std::size_t generation{};
};

struct worker_signals final {
    std::atomic<bool> stop{false};
    std::atomic<bool> pin_requested{false};
    std::atomic<bool> release_pin{false};
};

struct worker_outcomes final {
    std::atomic<bool> failed{false};
    std::atomic<bool> publisher_finished{false};
    std::atomic<bool> publisher_saw_shutdown{false};
    std::atomic<bool> pin_captured{false};
    std::atomic<bool> pin_read_rejected{false};
    std::atomic<bool> pin_released{false};
    std::atomic<bool> shutdown_entered{false};
    std::atomic<bool> shutdown_finished{false};
    std::atomic<bool> shutdown_failed{false};
    std::array<std::atomic<bool>, generation_count> publisher_progress{};
    std::array<std::array<std::atomic<bool>, generation_count>, reader_count> reader_progress{};
    std::array<std::atomic<bool>, reader_count> reader_saw_shutdown{};
    std::array<std::atomic<bool>, reader_count> reader_finished{};
    connection_target pin_target{};
};

[[nodiscard]] binding_view current_view(current_connection& current) {
    std::scoped_lock lock{current.mutex};
    return binding_view{
        .target = current.value.target,
        .reader = current.value.reader,
        .producer = current.value.producer,
        .generation = current.generation,
    };
}

[[nodiscard]] bool wait_for_flag(const std::atomic<bool>& flag,
    std::chrono::steady_clock::time_point deadline) noexcept {
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

[[nodiscard]] bool wait_for_progress(std::size_t generation,
    const std::array<std::atomic<bool>, generation_count>& publisher_progress,
    const std::array<std::array<std::atomic<bool>, generation_count>, reader_count>&
        reader_progress,
    const std::atomic<bool>& worker_failed,
    std::chrono::steady_clock::time_point deadline) noexcept {
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker_failed.load(std::memory_order_acquire)) {
            return false;
        }
        bool all_readers_progressed = true;
        for (const auto& progress : reader_progress) {
            all_readers_progressed =
                all_readers_progressed && progress[generation].load(std::memory_order_acquire);
        }
        if (publisher_progress[generation].load(std::memory_order_acquire) &&
            all_readers_progressed) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void wait_for_pin_request(worker_signals& signals) noexcept {
    auto requested = signals.pin_requested.load(std::memory_order_acquire);
    while (!requested && !signals.stop.load(std::memory_order_acquire)) {
        signals.pin_requested.wait(false, std::memory_order_acquire);
        requested = signals.pin_requested.load(std::memory_order_acquire);
    }
}

void wait_for_pin_release(worker_signals& signals) noexcept {
    auto should_release = signals.release_pin.load(std::memory_order_acquire);
    while (!should_release) {
        signals.release_pin.wait(false, std::memory_order_acquire);
        should_release = signals.release_pin.load(std::memory_order_acquire);
    }
}

class worker_cleanup final {
public:
    worker_cleanup(start_gate& start,
        worker_signals& signals,
        std::array<std::jthread, reader_count + 2U>& workers,
        std::jthread& shutdown_worker) noexcept
        : start_(start),
          signals_(signals),
          workers_(workers),
          shutdown_worker_(shutdown_worker) {}

    ~worker_cleanup() {
        signals_.stop.store(true, std::memory_order_release);
        start_.release();
        signals_.pin_requested.store(true, std::memory_order_release);
        signals_.pin_requested.notify_all();
        signals_.release_pin.store(true, std::memory_order_release);
        signals_.release_pin.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (shutdown_worker_.joinable()) {
            shutdown_worker_.join();
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    start_gate& start_;
    worker_signals& signals_;
    std::array<std::jthread, reader_count + 2U>& workers_;
    std::jthread& shutdown_worker_;
};

struct lifecycle_worker_state final {
    device_registry& registry;
    current_connection& current;
    start_gate& start;
    worker_signals& signals;
    worker_outcomes& outcomes;
    std::atomic<bool>& shutdown_started;

    void fail() noexcept { outcomes.failed.store(true, std::memory_order_release); }

    [[nodiscard]] bool publish_once(connection_target& last_target,
        std::uint64_t& next_revision) noexcept {
        const auto view = current_view(current);
        if (view.target != last_target) {
            last_target = view.target;
            next_revision = 1U;
        }
        if (!view.target.valid() || view.generation >= generation_count) {
            fail();
            return false;
        }
        try {
            const auto result = view.producer.publish(make_update(next_revision));
            if (result.has_value()) {
                if (result.value() != next_revision) {
                    fail();
                    return false;
                }
                outcomes.publisher_progress[view.generation].store(true, std::memory_order_release);
                ++next_revision;
                return true;
            }
            if (result.error() != error::not_connected) {
                fail();
                return false;
            }
            if (shutdown_started.load(std::memory_order_acquire) &&
                view.generation + 1U == generation_count) {
                outcomes.publisher_saw_shutdown.store(true, std::memory_order_release);
                return false;
            }
        } catch (...) {
            fail();
            return false;
        }
        return true;
    }

    void publisher() noexcept {
        start.arrive_and_wait();
        auto last_target = current_view(current).target;
        std::uint64_t next_revision = 2U;
        while (!signals.stop.load(std::memory_order_acquire)) {
            if (!publish_once(last_target, next_revision)) {
                break;
            }
            std::this_thread::yield();
        }
        outcomes.publisher_finished.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool read_once(std::size_t reader_index) noexcept {
        const auto view = current_view(current);
        if (!view.target.valid() || view.generation >= generation_count) {
            fail();
            return false;
        }
        try {
            const auto result = view.reader.read();
            if (result.has_value()) {
                if (result->target != view.target || !coherent(result.value())) {
                    fail();
                    return false;
                }
                outcomes.reader_progress[reader_index][view.generation].store(true,
                    std::memory_order_release);
                return true;
            }
            if (result.error() == error::no_data) {
                return true;
            }
            if (result.error() != error::not_connected) {
                fail();
                return false;
            }
            if (shutdown_started.load(std::memory_order_acquire) &&
                view.generation + 1U == generation_count) {
                outcomes.reader_saw_shutdown[reader_index].store(true, std::memory_order_release);
                return false;
            }
        } catch (...) {
            fail();
            return false;
        }
        return true;
    }

    void reader(std::size_t reader_index) noexcept {
        start.arrive_and_wait();
        while (!signals.stop.load(std::memory_order_acquire)) {
            if (!read_once(reader_index)) {
                break;
            }
            std::this_thread::yield();
        }
        outcomes.reader_finished[reader_index].store(true, std::memory_order_release);
    }

    void pin_holder() noexcept {
        start.arrive_and_wait();
        wait_for_pin_request(signals);
        if (signals.stop.load(std::memory_order_acquire)) {
            return;
        }
        const auto view = current_view(current);
        auto result = view.reader.capture();
        if (!result.has_value()) {
            fail();
            return;
        }
        auto pin = std::move(*result);
        outcomes.pin_target = pin.target();
        outcomes.pin_captured.store(true, std::memory_order_release);
        outcomes.pin_captured.notify_all();

        wait_for_pin_release(signals);
        const auto read_after_shutdown = pin.read();
        outcomes.pin_read_rejected.store(!read_after_shutdown.has_value() &&
                                             read_after_shutdown.error() == error::not_connected,
            std::memory_order_release);
        pin.release();
        outcomes.pin_released.store(true, std::memory_order_release);
        outcomes.pin_released.notify_all();
    }

    void shutdown(std::thread::id& thread_id) noexcept {
        thread_id = std::this_thread::get_id();
        outcomes.shutdown_entered.store(true, std::memory_order_release);
        outcomes.shutdown_entered.notify_all();
        try {
            registry.shutdown();
        } catch (...) {
            outcomes.shutdown_failed.store(true, std::memory_order_release);
        }
        outcomes.shutdown_finished.store(true, std::memory_order_release);
        outcomes.shutdown_finished.notify_all();
    }
};

void start_workers(lifecycle_worker_state& state,
    std::array<std::jthread, reader_count + 2U>& workers) {
    workers[0U] = std::jthread{[&state] { state.publisher(); }};
    for (std::size_t index = 0U; index < reader_count; ++index) {
        workers[index + 1U] = std::jthread{[&state, index] { state.reader(index); }};
    }
    workers[reader_count + 1U] = std::jthread{[&state] { state.pin_holder(); }};
}

[[nodiscard]] bool wait_for_shutdown_observation(const worker_outcomes& outcomes,
    std::chrono::steady_clock::time_point deadline) noexcept {
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_readers_observed_shutdown = true;
        for (const auto& observed : outcomes.reader_saw_shutdown) {
            all_readers_observed_shutdown =
                all_readers_observed_shutdown && observed.load(std::memory_order_acquire);
        }
        if (outcomes.publisher_saw_shutdown.load(std::memory_order_acquire) &&
            all_readers_observed_shutdown) {
            return true;
        }
        if (outcomes.failed.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::yield();
    }
    return false;
}

void join_workers(std::array<std::jthread, reader_count + 2U>& workers,
    std::jthread& shutdown_worker) noexcept {
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    if (shutdown_worker.joinable()) {
        shutdown_worker.join();
    }
}

TEST_CASE("devices.stress.concurrent_lifecycle", "[component][devices][stress][lifecycle]") {
    constexpr std::uint64_t seed = 0xA05A06A07U;
    CAPTURE(seed, churn_rounds, reader_count);

    destruction_observer destroyed;
    device_registry registry{make_runtime(static_cast<std::byte>(seed & 0xFFU))};
    auto first_source = make_source(&destroyed);
    auto first = registry.admit(first_source);
    REQUIRE(first.has_value());
    REQUIRE(first->producer.publish(make_update(1U)).value() == 1U);

    current_connection current;
    current.value = first.value();
    std::atomic<bool> shutdown_started{false};
    worker_signals signals;
    worker_outcomes outcomes;
    start_gate start;
    lifecycle_worker_state worker_state{
        .registry = registry,
        .current = current,
        .start = start,
        .signals = signals,
        .outcomes = outcomes,
        .shutdown_started = shutdown_started,
    };
    std::thread::id shutdown_thread_id{};
    std::array<std::jthread, reader_count + 2U> workers{};
    std::jthread shutdown_worker;
    const worker_cleanup cleanup{start, signals, workers, shutdown_worker};

    start_workers(worker_state, workers);
    start.wait_for_ready(workers.size());
    start.release();

    const auto operation_deadline = std::chrono::steady_clock::now() + 20s;
    bool every_generation_progressed = true;
    bool every_replacement_succeeded = true;
    for (std::size_t generation = 0U; generation < generation_count; ++generation) {
        if (!wait_for_progress(generation,
                outcomes.publisher_progress,
                outcomes.reader_progress,
                outcomes.failed,
                operation_deadline)) {
            every_generation_progressed = false;
            break;
        }
        if (generation + 1U == generation_count) {
            break;
        }

        const auto old = current_view(current);
        auto replacement_source = make_source(&destroyed);
        auto replacement = registry.replace(old.target, replacement_source);
        if (!replacement.has_value()) {
            every_replacement_succeeded = false;
            break;
        }
        {
            std::scoped_lock lock{current.mutex};
            current.value = replacement.value();
            current.generation = generation + 1U;
        }
    }

    REQUIRE(every_generation_progressed);
    REQUIRE(every_replacement_succeeded);
    REQUIRE(!outcomes.failed.load(std::memory_order_acquire));

    signals.pin_requested.store(true, std::memory_order_release);
    signals.pin_requested.notify_all();
    REQUIRE(wait_for_flag(outcomes.pin_captured, operation_deadline));
    const auto final_target = current_view(current).target;

    shutdown_started.store(true, std::memory_order_release);
    shutdown_worker = std::jthread{[&] { worker_state.shutdown(shutdown_thread_id); }};

    const auto shutdown_started_in_worker =
        wait_for_flag(outcomes.shutdown_entered, operation_deadline);
    const auto io_workers_observed_shutdown =
        wait_for_shutdown_observation(outcomes, operation_deadline);
    const auto shutdown_waited_for_pin =
        !outcomes.shutdown_finished.load(std::memory_order_acquire);
    const auto resident_while_pin_held = registry.resident_count();
    const auto destructor_calls_while_pin_held = destroyed.call_count();
    const auto pin_target_matches = outcomes.pin_target == final_target;

    signals.release_pin.store(true, std::memory_order_release);
    signals.release_pin.notify_all();
    const auto pin_was_released = wait_for_flag(outcomes.pin_released, operation_deadline);
    const auto shutdown_completed_before_join =
        wait_for_flag(outcomes.shutdown_finished, operation_deadline);
    signals.stop.store(true, std::memory_order_release);
    join_workers(workers, shutdown_worker);

    const auto shutdown_completed = outcomes.shutdown_finished.load(std::memory_order_acquire);
    const auto live_after_shutdown = registry.live_count();
    const auto residents_after_shutdown = registry.resident_count();
    const auto destructor_calls_after_shutdown = destroyed.call_count();
    const auto destructor_thread_after_shutdown = destroyed.thread_id();

    CHECK(shutdown_started_in_worker);
    CHECK(io_workers_observed_shutdown);
    CHECK(shutdown_waited_for_pin);
    CHECK(resident_while_pin_held >= 1U);
    CHECK(destructor_calls_while_pin_held < generation_count);
    CHECK(pin_was_released);
    CHECK(outcomes.pin_read_rejected.load(std::memory_order_acquire));
    CHECK(pin_target_matches);
    CHECK(shutdown_completed_before_join);
    CHECK(shutdown_completed);
    CHECK(!outcomes.failed.load(std::memory_order_acquire));
    CHECK(!outcomes.shutdown_failed.load(std::memory_order_acquire));
    CHECK(outcomes.publisher_finished.load(std::memory_order_acquire));
    for (const auto& finished : outcomes.reader_finished) {
        CHECK(finished.load(std::memory_order_acquire));
    }
    CHECK(live_after_shutdown == 0U);
    CHECK(residents_after_shutdown == 0U);
    CHECK(destructor_calls_after_shutdown == generation_count);
    CHECK(destructor_thread_after_shutdown == shutdown_thread_id);
}

} // namespace
