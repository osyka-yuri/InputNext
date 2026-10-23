#include <inputnext/device_registry.hpp>

#include "test_support.hpp"
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;
using namespace std::chrono_literals;

class blocking_gate final {
public:
    blocking_gate() = default;

    ~blocking_gate() { release(); }

    blocking_gate(const blocking_gate&) = delete;
    blocking_gate& operator=(const blocking_gate&) = delete;
    blocking_gate(blocking_gate&&) = delete;
    blocking_gate& operator=(blocking_gate&&) = delete;

    void pause() {
        std::unique_lock lock{mutex_};
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool wait_until_entered() {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 2s, [this] { return entered_; });
    }

    void release() noexcept {
        std::scoped_lock lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

class worker_cleanup final {
public:
    explicit worker_cleanup(std::jthread& first,
        blocking_gate* gate = nullptr,
        read_pin* pin = nullptr,
        std::jthread* second = nullptr) noexcept
        : first_(first),
          gate_(gate),
          pin_(pin),
          second_(second) {}

    ~worker_cleanup() {
        if (gate_ != nullptr) {
            gate_->release();
        }
        if (pin_ != nullptr) {
            pin_->release();
        }
        if (first_.joinable()) {
            first_.join();
        }
        if (second_ != nullptr && second_->joinable()) {
            second_->join();
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    std::jthread& first_;
    blocking_gate* gate_{};
    read_pin* pin_{};
    std::jthread* second_{};
};

class gate_release_guard final {
public:
    explicit gate_release_guard(blocking_gate& gate) noexcept : gate_(gate) {}

    ~gate_release_guard() { gate_.release(); }

    gate_release_guard(const gate_release_guard&) = delete;
    gate_release_guard& operator=(const gate_release_guard&) = delete;
    gate_release_guard(gate_release_guard&&) = delete;
    gate_release_guard& operator=(gate_release_guard&&) = delete;

private:
    blocking_gate& gate_;
};

class blocking_source final : public source_lifetime {
public:
    blocking_source(blocking_gate& gate, destruction_observer& observer) noexcept
        : gate_(gate),
          observer_(observer) {}

    ~blocking_source() noexcept override {
        observer_.record();
        gate_.pause();
    }

    blocking_source(const blocking_source&) = delete;
    blocking_source& operator=(const blocking_source&) = delete;
    blocking_source(blocking_source&&) = delete;
    blocking_source& operator=(blocking_source&&) = delete;

private:
    blocking_gate& gate_;
    destruction_observer& observer_;
};

TEST_CASE("devices.retirement_with_pinned_reader", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    REQUIRE(admitted->producer.publish(make_update(1U)).has_value());

    blocking_gate pin_held;
    std::atomic<bool> capture_succeeded{};
    std::atomic<bool> post_retirement_read_rejected{};
    std::jthread reader_worker;
    const worker_cleanup cleanup{reader_worker, &pin_held};
    const auto reader_binding = admitted->reader;
    reader_worker = std::jthread{
        [reader_binding, &pin_held, &capture_succeeded, &post_retirement_read_rejected] {
            try {
                auto pin = reader_binding.capture();
                if (!pin.has_value()) {
                    return;
                }
                capture_succeeded.store(true, std::memory_order_release);
                pin_held.pause();
                const auto read = pin->read();
                post_retirement_read_rejected.store(!read.has_value() &&
                                                        read.error() == error::not_connected,
                    std::memory_order_release);
                pin->release();
            } catch (...) {
                post_retirement_read_rejected.store(false, std::memory_order_release);
            }
        }};

    REQUIRE(pin_held.wait_until_entered());
    REQUIRE(registry.retire(admitted->target).has_value());
    const auto new_capture = admitted->reader.capture();
    REQUIRE(!new_capture.has_value());
    CHECK(new_capture.error() == error::not_connected);
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 1U);
    CHECK(destroyed.call_count() == 0U);

    pin_held.release();
    reader_worker.join();
    CHECK(capture_succeeded.load(std::memory_order_acquire));
    CHECK(post_retirement_read_rejected.load(std::memory_order_acquire));
    CHECK(destroyed.call_count() == 0U);
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 1U);
}

TEST_CASE("devices.stale_producer_after_replacement", "[component][devices][concurrency]") {
    device_registry registry{make_runtime()};
    auto source = make_source();
    auto original = registry.admit(source);
    REQUIRE(original.has_value());

    blocking_gate producer_ready;
    std::atomic<bool> worker_started{};
    std::atomic<bool> stale_publish_rejected{};
    std::jthread producer_worker;
    const worker_cleanup cleanup{producer_worker, &producer_ready};
    const auto stale_producer = original->producer;
    producer_worker =
        std::jthread{[stale_producer, &producer_ready, &worker_started, &stale_publish_rejected] {
            worker_started.store(true, std::memory_order_release);
            try {
                producer_ready.pause();
                const auto publication = stale_producer.publish(make_update(7U));
                stale_publish_rejected.store(!publication.has_value() &&
                                                 publication.error() == error::not_connected,
                    std::memory_order_release);
            } catch (...) {
                stale_publish_rejected.store(false, std::memory_order_release);
            }
        }};

    REQUIRE(producer_ready.wait_until_entered());
    auto replacement_source = make_source();
    auto replacement = registry.replace(original->target, replacement_source);
    REQUIRE(replacement.has_value());
    producer_ready.release();
    producer_worker.join();

    CHECK(worker_started.load(std::memory_order_acquire));
    CHECK(stale_publish_rejected.load(std::memory_order_acquire));
    const auto replacement_update = make_update(1U);
    const auto publication = replacement->producer.publish(replacement_update);
    REQUIRE(publication.has_value());
    CHECK(publication.value() == 1U);
    const auto current = replacement->reader.read();
    REQUIRE(current.has_value());
    CHECK(current->target == replacement->target);
    CHECK(current->sample.state == replacement_update.state);
    CHECK(current->sample.revision == 1U);
}

TEST_CASE("devices.last_pin_release_defers_destruction", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    auto pin_result = admitted->reader.capture();
    REQUIRE(pin_result.has_value());
    auto pin = std::move(pin_result.value());
    REQUIRE(registry.retire(admitted->target).has_value());

    std::atomic<bool> released{};
    std::jthread release_worker{[pin = std::move(pin), &released] mutable {
        pin.release();
        released.store(true, std::memory_order_release);
    }};
    release_worker.join();

    CHECK(released.load(std::memory_order_acquire));
    CHECK(destroyed.call_count() == 0U);
    CHECK(registry.resident_count() == 1U);
    const auto drain_thread = std::this_thread::get_id();
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 1U);
    CHECK(destroyed.thread_id() == drain_thread);
}

TEST_CASE("devices.move_only_pin_ownership", "[component][devices]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    read_pin surviving_pin;
    connection_target surviving_target{};

    {
        auto first_source = make_source(&destroyed);
        auto first = registry.admit(first_source);
        REQUIRE(first.has_value());
        auto first_pin_result = first->reader.capture();
        REQUIRE(first_pin_result.has_value());
        auto first_pin = std::move(first_pin_result.value());
        read_pin moved_pin{std::move(first_pin)};
        CHECK(moved_pin.target() == first->target);
        REQUIRE(registry.retire(first->target).has_value());

        auto second_source = make_source(&destroyed);
        auto second = registry.admit(second_source);
        REQUIRE(second.has_value());
        auto second_pin_result = second->reader.capture();
        REQUIRE(second_pin_result.has_value());
        auto second_pin = std::move(second_pin_result.value());
        read_pin occupied_pin{std::move(second_pin)};
        CHECK(occupied_pin.target() == second->target);
        REQUIRE(registry.retire(second->target).has_value());

        moved_pin = std::move(occupied_pin);
        CHECK(moved_pin.target() == second->target);
        CHECK(destroyed.call_count() == 0U);
        CHECK(registry.resident_count() == 2U);

        CHECK(registry.drain_retired() == 1U);
        CHECK(destroyed.call_count() == 1U);
        CHECK(registry.resident_count() == 1U);

        moved_pin.release();
        CHECK(destroyed.call_count() == 1U);
        CHECK(registry.resident_count() == 1U);
        CHECK(registry.drain_retired() == 1U);
        CHECK(destroyed.call_count() == 2U);
        CHECK(registry.resident_count() == 0U);

        auto recovered_source = make_source(&destroyed);
        auto recovered = registry.admit(recovered_source);
        REQUIRE(recovered.has_value());
        surviving_target = recovered->target;
        auto recovered_pin = recovered->reader.capture();
        REQUIRE(recovered_pin.has_value());
        surviving_pin = std::move(recovered_pin.value());
        CHECK(surviving_pin.target() == surviving_target);
        CHECK(registry.live_count() == 1U);
        CHECK(registry.resident_count() == 1U);
    }

    REQUIRE(registry.retire(surviving_target).has_value());
    CHECK(registry.drain_retired() == 0U);
    CHECK(destroyed.call_count() == 2U);
    surviving_pin.release();
    CHECK(destroyed.call_count() == 2U);
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 3U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.reuse_rejects_old_binding", "[component][devices][concurrency]") {
    device_registry registry{make_runtime()};
    auto old_source = make_source();
    auto old = registry.admit(old_source);
    REQUIRE(old.has_value());
    const auto stale_reader = old->reader;
    const auto stale_producer = old->producer;
    REQUIRE(registry.retire(old->target).has_value());
    CHECK(registry.drain_retired() == 1U);

    auto new_source = make_source();
    auto current = registry.admit(new_source);
    REQUIRE(current.has_value());
    REQUIRE(current->producer.publish(make_update(1U)).has_value());

    std::atomic<bool> stale_read_rejected{};
    std::atomic<bool> stale_publish_rejected{};
    std::jthread stale_worker{
        [stale_reader, stale_producer, &stale_read_rejected, &stale_publish_rejected] {
            const auto read = stale_reader.read();
            const auto publication = stale_producer.publish(make_update(2U));
            stale_read_rejected.store(!read.has_value() && read.error() == error::not_connected,
                std::memory_order_release);
            stale_publish_rejected.store(!publication.has_value() &&
                                             publication.error() == error::not_connected,
                std::memory_order_release);
        }};
    stale_worker.join();

    CHECK(stale_read_rejected.load(std::memory_order_acquire));
    CHECK(stale_publish_rejected.load(std::memory_order_acquire));
    const auto current_read = current->reader.read();
    REQUIRE(current_read.has_value());
    CHECK(current_read->target == current->target);
    CHECK(current_read->sample.state == make_update(1U).state);
    CHECK(current_read->sample.revision == 1U);
}

TEST_CASE("devices.slow_destruction_keeps_ready_reads_progressing",
    "[component][devices][concurrency]") {
    blocking_gate destruction_gate;
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    std::unique_ptr<source_lifetime> source;
    const gate_release_guard release_gate{destruction_gate};

    source = std::make_unique<blocking_source>(destruction_gate, destroyed);
    auto retired = registry.admit(source);
    REQUIRE(retired.has_value());
    REQUIRE(registry.retire(retired->target).has_value());

    auto live_source = make_source();
    auto live = registry.admit(live_source);
    REQUIRE(live.has_value());
    REQUIRE(live->producer.publish(make_update(1U)).has_value());

    std::thread::id drain_thread{};
    std::promise<bool> read_result;
    auto read_future = read_result.get_future();
    std::jthread drain_worker;
    std::jthread read_worker;
    const worker_cleanup cleanup{drain_worker, &destruction_gate, nullptr, &read_worker};
    drain_worker = std::jthread{[&registry, &drain_thread] {
        drain_thread = std::this_thread::get_id();
        static_cast<void>(registry.drain_retired());
    }};

    const auto destruction_entered = destruction_gate.wait_until_entered();
    const auto destroyed_while_blocked = destroyed.call_count();
    const auto residents_while_blocked = registry.resident_count();
    const auto live_reader = live->reader;
    read_worker = std::jthread{[live_reader, &read_result] {
        try {
            const auto read = live_reader.read();
            read_result.set_value(read.has_value() && coherent(read.value()));
        } catch (...) {
            read_result.set_value(false);
        }
    }};

    const auto read_status_while_blocked = read_future.wait_for(2s);
    const auto reader_progressed_while_blocked =
        read_status_while_blocked == std::future_status::ready;

    destruction_gate.release();
    drain_worker.join();
    read_worker.join();
    const auto reader_result = read_future.get();
    const auto residents_after_drain = registry.resident_count();
    const auto destructor_calls_after_drain = destroyed.call_count();
    const auto destructor_thread_after_drain = destroyed.thread_id();

    CHECK(destruction_entered);
    CHECK(destroyed_while_blocked == 1U);
    CHECK(residents_while_blocked == 2U);
    CHECK(reader_progressed_while_blocked);
    CHECK(reader_result);
    CHECK(residents_after_drain == 1U);
    CHECK(destructor_calls_after_drain == 1U);
    CHECK(destructor_thread_after_drain == drain_thread);
}

TEST_CASE("devices.shutdown_with_inflight_pin", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    auto pin_result = registry.capture(admitted->target);
    REQUIRE(pin_result.has_value());
    auto pin = std::move(pin_result.value());

    std::thread::id shutdown_thread{};
    std::atomic<bool> shutdown_finished{};
    std::jthread shutdown_worker;
    const worker_cleanup cleanup{shutdown_worker, nullptr, &pin};
    shutdown_worker = std::jthread{[&registry, &shutdown_thread, &shutdown_finished] {
        shutdown_thread = std::this_thread::get_id();
        try {
            registry.shutdown();
            shutdown_finished.store(true, std::memory_order_release);
        } catch (...) {
            shutdown_finished.store(false, std::memory_order_release);
        }
    }};

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool observed_revocation = false;
    while (std::chrono::steady_clock::now() < deadline) {
        auto probe = registry.capture(admitted->target);
        if (!probe.has_value()) {
            observed_revocation = probe.error() == error::not_connected;
            break;
        }
        probe->release();
        std::this_thread::yield();
    }

    const auto shutdown_waited_for_pin = !shutdown_finished.load(std::memory_order_acquire);
    const auto residents_while_shutdown_waited = registry.resident_count();
    const auto pin_read = pin.read();
    const auto pin_read_was_revoked = !pin_read.has_value();
    const auto pin_read_error = pin_read_was_revoked ? pin_read.error() : error::invalid_argument;

    pin.release();
    shutdown_worker.join();
    const auto shutdown_completed_after_release = shutdown_finished.load(std::memory_order_acquire);
    const auto live_after_shutdown = registry.live_count();
    const auto residents_after_shutdown = registry.resident_count();
    const auto destructor_calls_after_shutdown = destroyed.call_count();
    const auto destructor_thread_after_shutdown = destroyed.thread_id();

    auto rejected_source = make_source();
    auto* const rejected_pointer = rejected_source.get();
    const auto rejected = registry.admit(rejected_source);
    const auto rejected_after_shutdown = !rejected.has_value();
    const auto rejection_error =
        rejected_after_shutdown ? rejected.error() : error::invalid_argument;

    CHECK(observed_revocation);
    CHECK(shutdown_waited_for_pin);
    CHECK(residents_while_shutdown_waited == 1U);
    CHECK(pin_read_was_revoked);
    CHECK(pin_read_error == error::not_connected);
    CHECK(shutdown_completed_after_release);
    CHECK(live_after_shutdown == 0U);
    CHECK(residents_after_shutdown == 0U);
    CHECK(destructor_calls_after_shutdown == 1U);
    CHECK(destructor_thread_after_shutdown == shutdown_thread);
    CHECK(rejected_after_shutdown);
    CHECK(rejection_error == error::closed);
    CHECK(rejected_source.get() == rejected_pointer);
}

TEST_CASE("devices.shutdown_after_last_pin_release_does_not_wait_for_a_notification",
    "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    auto pin = registry.capture(admitted->target);
    REQUIRE(pin.has_value());

    pin->release();
    registry.shutdown();

    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 0U);
    CHECK(destroyed.call_count() == 1U);
}

} // namespace
