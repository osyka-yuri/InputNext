#include <inputnext/device_registry.hpp>

#include "connection_record.hpp"
#include "test_support.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/latest_value.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::detail;
using namespace inputnext::test::devices;
using namespace std::chrono_literals;

[[nodiscard]] connection_target make_target() noexcept {
    return connection_target{
        .runtime = make_runtime(),
        .device = device_key{1U},
        .connection = connection_id{1U},
    };
}

[[nodiscard]] latest_value& require_latest(connection_record& record) {
    if (record.latest.has_value()) {
        return *record.latest;
    }
    throw std::runtime_error{"pin-release fixture requires an engaged latest value"};
}

TEST_CASE("devices.pin_release_wait_accepts_zero_before_wait_registration",
    "[component][devices][concurrency]") {
    connection_record record;
    record.phase.store(connection_phase::retired, std::memory_order_seq_cst);
    record.pin_count.store(1U, std::memory_order_seq_cst);
    const auto observed = record.pin_count.load(std::memory_order_seq_cst);

    REQUIRE(observed == 1U);
    CHECK(record.release_pin());
    record.wait_for_pins(observed);
    record.notify_pin_release();

    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 0U);
}

TEST_CASE("devices.pin_release_delayed_tail_is_safe_after_record_reuse",
    "[component][devices][concurrency]") {
    destruction_observer destroyed;
    connection_record record;
    const auto old_target = make_target();
    auto live_target = old_target;
    live_target.connection = connection_id{2U};
    auto retired_target = old_target;
    retired_target.connection = connection_id{3U};
    const auto live_payload = make_update(73U);
    const auto retired_payload = make_update(91U);
    const auto payload_matches = [](const published_sample& sample, const input_update& update) {
        return sample.state == update.state &&
               sample.source_timestamp_us == update.source_timestamp_us &&
               sample.observation_qpc == update.observation_qpc;
    };

    {
        std::scoped_lock lock{record.gate};
        record.target = old_target;
        record.pin_count.store(1U, std::memory_order_seq_cst);
        record.latest.emplace(0U);
        record.source = make_source(&destroyed);
        record.phase.store(connection_phase::live, std::memory_order_seq_cst);
    }

    // Keep this final release's notification tail pending while its slot is reclaimed and reused.
    CHECK(record.release_pin());
    {
        std::scoped_lock lock{record.gate};
        require_latest(record).close();
        record.phase.store(connection_phase::retired, std::memory_order_seq_cst);
    }
    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 0U);

    std::unique_ptr<source_lifetime> reclaimed_source;
    {
        std::scoped_lock lock{record.gate};
        record.phase.store(connection_phase::draining, std::memory_order_seq_cst);
        reclaimed_source = std::move(record.source);
    }
    reclaimed_source.reset();
    CHECK(destroyed.call_count() == 1U);

    {
        std::scoped_lock lock{record.gate};
        record.latest.reset();
        record.target = {};
        record.phase.store(connection_phase::free, std::memory_order_seq_cst);

        record.phase.store(connection_phase::reserved, std::memory_order_seq_cst);
        record.target = live_target;
        record.pin_count.store(1U, std::memory_order_seq_cst);
        record.latest.emplace(0U);
        record.source = make_source(&destroyed);
        REQUIRE(require_latest(record).publish(live_payload).has_value());
        record.phase.store(connection_phase::live, std::memory_order_seq_cst);
    }
    auto* const live_source = record.source.get();
    auto* const live_latest = &require_latest(record);

    // The old tail sees a live replacement and must leave its logical pin and payload alone.
    record.notify_pin_release();
    const auto live_sample = require_latest(record).read();
    REQUIRE(live_sample.has_value());
    CHECK(payload_matches(*live_sample, live_payload));
    CHECK(record.phase.load(std::memory_order_seq_cst) == connection_phase::live);
    CHECK(record.target == live_target);
    CHECK(record.source.get() == live_source);
    CHECK(&require_latest(record) == live_latest);
    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 1U);
    CHECK(destroyed.call_count() == 1U);

    // Leave a second final-release tail pending while the live replacement is reclaimed.
    CHECK(record.release_pin());
    {
        std::scoped_lock lock{record.gate};
        require_latest(record).close();
        record.phase.store(connection_phase::retired, std::memory_order_seq_cst);
    }
    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 0U);
    {
        std::scoped_lock lock{record.gate};
        record.phase.store(connection_phase::draining, std::memory_order_seq_cst);
        reclaimed_source = std::move(record.source);
    }
    reclaimed_source.reset();
    CHECK(destroyed.call_count() == 2U);

    {
        std::scoped_lock lock{record.gate};
        record.latest.reset();
        record.target = {};
        record.phase.store(connection_phase::free, std::memory_order_seq_cst);

        record.phase.store(connection_phase::reserved, std::memory_order_seq_cst);
        record.target = retired_target;
        record.pin_count.store(1U, std::memory_order_seq_cst);
        record.latest.emplace(0U);
        record.source = make_source(&destroyed);
        REQUIRE(require_latest(record).publish(retired_payload).has_value());
        record.phase.store(connection_phase::live, std::memory_order_seq_cst);
    }
    auto* const retired_source = record.source.get();
    auto* const retired_latest = &require_latest(record);
    const auto retired_sample = require_latest(record).read();
    REQUIRE(retired_sample.has_value());
    CHECK(payload_matches(*retired_sample, retired_payload));
    {
        std::scoped_lock lock{record.gate};
        require_latest(record).close();
        record.phase.store(connection_phase::retired, std::memory_order_seq_cst);
    }

    // The prior incarnation's tail may notify but cannot reclaim or mutate this positive pin.
    record.notify_pin_release();
    const auto closed_sample = require_latest(record).read();
    CHECK(!closed_sample.has_value());
    if (!closed_sample.has_value()) {
        CHECK(closed_sample.error() == error::not_connected);
    }
    CHECK(record.phase.load(std::memory_order_seq_cst) == connection_phase::retired);
    CHECK(record.target == retired_target);
    CHECK(record.source.get() == retired_source);
    CHECK(&require_latest(record) == retired_latest);
    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 1U);
    CHECK(destroyed.call_count() == 2U);

    CHECK(record.release_pin());
    record.notify_pin_release();
    record.wait_for_pins(1U);
    CHECK(record.pin_count.load(std::memory_order_seq_cst) == 0U);
}

TEST_CASE("devices.pin_release_wait_publishes_all_owner_payloads_before_join",
    "[component][devices][concurrency]") {
    constexpr std::size_t owner_count = 4U;
    connection_record record;
    record.phase.store(connection_phase::retired, std::memory_order_seq_cst);
    record.pin_count.store(owner_count, std::memory_order_seq_cst);
    std::array<std::size_t, owner_count> payload{};
    std::array<std::jthread, owner_count> owners{};

    for (std::size_t index = 0U; index < owner_count; ++index) {
        owners[index] = std::jthread{[&record, &payload, index] {
            payload[index] = index + 1U;
            if (record.release_pin()) {
                record.notify_pin_release();
            }
        }};
    }

    record.wait_for_pins(owner_count);
    for (std::size_t index = 0U; index < owner_count; ++index) {
        CHECK(payload[index] == index + 1U);
    }

    for (auto& owner : owners) {
        owner.join();
    }
}

TEST_CASE("devices.pin_count_release_preserves_full_size_t_range",
    "[component][devices][concurrency]") {
    connection_record record;
    record.pin_count.store(std::numeric_limits<std::size_t>::max(), std::memory_order_seq_cst);

    CHECK(!record.release_pin());
    CHECK(record.pin_count.load(std::memory_order_seq_cst) ==
          std::numeric_limits<std::size_t>::max() - 1U);

    CHECK(!record.release_pin());
    CHECK(record.pin_count.load(std::memory_order_seq_cst) ==
          std::numeric_limits<std::size_t>::max() - 2U);
}

class shutdown_cleanup final {
public:
    shutdown_cleanup(std::jthread& worker, std::array<read_pin, 3U>& pins) noexcept
        : worker_(worker),
          pins_(pins) {}

    ~shutdown_cleanup() {
        for (auto& pin : pins_) {
            pin.release();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    shutdown_cleanup(const shutdown_cleanup&) = delete;
    shutdown_cleanup& operator=(const shutdown_cleanup&) = delete;
    shutdown_cleanup(shutdown_cleanup&&) = delete;
    shutdown_cleanup& operator=(shutdown_cleanup&&) = delete;

private:
    std::jthread& worker_;
    std::array<read_pin, 3U>& pins_;
};

TEST_CASE("devices.shutdown_waits_for_partial_multipin_release_and_destroys_on_caller",
    "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());

    std::array<read_pin, 3U> pins{};
    for (auto& pin : pins) {
        auto captured = registry.capture(admitted->target);
        REQUIRE(captured.has_value());
        pin = std::move(captured.value());
    }

    std::thread::id shutdown_thread{};
    std::atomic<bool> shutdown_finished{};
    std::atomic<bool> shutdown_succeeded{};
    std::jthread shutdown_worker;
    const shutdown_cleanup cleanup{shutdown_worker, pins};
    shutdown_worker =
        std::jthread{[&registry, &shutdown_thread, &shutdown_finished, &shutdown_succeeded] {
            shutdown_thread = std::this_thread::get_id();
            try {
                registry.shutdown();
                shutdown_succeeded.store(true, std::memory_order_release);
            } catch (...) {
                shutdown_succeeded.store(false, std::memory_order_release);
            }
            shutdown_finished.store(true, std::memory_order_release);
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

    pins[0].release();
    const auto waiting_after_first_release = !shutdown_finished.load(std::memory_order_acquire);
    const auto residents_after_first_release = registry.resident_count();
    const auto destructions_after_first_release = destroyed.call_count();
    pins[1].release();
    const auto waiting_after_second_release = !shutdown_finished.load(std::memory_order_acquire);
    const auto destructions_after_second_release = destroyed.call_count();
    pins[2].release();
    shutdown_worker.join();

    CHECK(observed_revocation);
    CHECK(waiting_after_first_release);
    CHECK(residents_after_first_release == 1U);
    CHECK(destructions_after_first_release == 0U);
    CHECK(waiting_after_second_release);
    CHECK(destructions_after_second_release == 0U);
    CHECK(shutdown_finished.load(std::memory_order_acquire));
    CHECK(shutdown_succeeded.load(std::memory_order_acquire));
    CHECK(registry.resident_count() == 0U);
    CHECK(destroyed.call_count() == 1U);
    CHECK(destroyed.thread_id() == shutdown_thread);
}

} // namespace
