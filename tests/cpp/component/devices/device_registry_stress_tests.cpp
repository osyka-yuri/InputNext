#include <inputnext/device_registry.hpp>

#include "test_start_gate.hpp"
#include "test_support.hpp"
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;

[[nodiscard]] std::uint64_t next_schedule_value(std::uint64_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

class worker_cleanup final {
public:
    worker_cleanup(start_gate& gate, std::jthread& first, std::jthread& second) noexcept
        : gate_(gate),
          first_(first),
          second_(second) {}

    ~worker_cleanup() {
        gate_.release();
        if (first_.joinable()) {
            first_.join();
        }
        if (second_.joinable()) {
            second_.join();
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    start_gate& gate_;
    std::jthread& first_;
    std::jthread& second_;
};

TEST_CASE("devices.stress.bounded_churn", "[component][devices][stress]") {
    constexpr std::uint64_t seed = 0x20261024U;
    constexpr std::uint64_t publication_count = 70000U;
    constexpr std::uint64_t reconnect_count = 300U;
    CAPTURE(seed, publication_count, reconnect_count);

    const auto runtime_first_byte = static_cast<std::byte>(seed & 0xFFU);
    destruction_observer destroyed;
    device_registry registry{make_runtime(runtime_first_byte)};
    auto source = make_source();
    auto first = registry.admit(source);
    REQUIRE(first.has_value());
    const auto initial = first->producer.publish(make_update(1U));
    REQUIRE(initial.has_value());
    CHECK(initial.value() == 1U);

    start_gate start;
    std::atomic<bool> publisher_ok{true};
    std::atomic<bool> reader_ok{true};
    std::atomic<std::size_t> coherent_reads{};
    std::jthread publisher;
    std::jthread reader;
    const worker_cleanup cleanup{start, publisher, reader};

    const auto producer = first->producer;
    publisher = std::jthread{[producer, &start, &publisher_ok] {
        start.arrive_and_wait();
        for (std::uint64_t revision = 2U; revision <= publication_count; ++revision) {
            try {
                const auto published = producer.publish(make_update(revision));
                if (!published.has_value() || published.value() != revision) {
                    publisher_ok.store(false, std::memory_order_relaxed);
                }
            } catch (...) {
                publisher_ok.store(false, std::memory_order_relaxed);
            }
        }
    }};
    const auto reader_binding = first->reader;
    reader = std::jthread{[reader_binding, &start, &reader_ok, &coherent_reads] {
        start.arrive_and_wait();
        for (std::uint64_t attempt = 0U; attempt < publication_count; ++attempt) {
            try {
                const auto snapshot = reader_binding.read();
                if (!snapshot.has_value() || !coherent(snapshot.value())) {
                    reader_ok.store(false, std::memory_order_relaxed);
                } else {
                    coherent_reads.fetch_add(1U, std::memory_order_relaxed);
                }
            } catch (...) {
                reader_ok.store(false, std::memory_order_relaxed);
            }
        }
    }};
    start.wait_for_ready(2U);
    start.release();
    publisher.join();
    reader.join();

    CHECK(publisher_ok.load(std::memory_order_relaxed));
    CHECK(reader_ok.load(std::memory_order_relaxed));
    CHECK(coherent_reads.load(std::memory_order_relaxed) == publication_count);
    const auto final_snapshot = first->reader.read();
    REQUIRE(final_snapshot.has_value());
    CHECK(final_snapshot->target == first->target);
    CHECK(final_snapshot->sample.revision == publication_count);
    CHECK(coherent(final_snapshot.value()));

    REQUIRE(registry.retire(first->target).has_value());
    CHECK(registry.drain_retired() == 1U);

    constexpr std::size_t resident_limit = 128U;
    constexpr std::size_t recovery_count = 3U;
    std::array<read_pin, resident_limit> retained_pins{};
    for (auto& retained_pin : retained_pins) {
        auto retained_source = make_source(&destroyed);
        auto retained = registry.admit(retained_source);
        REQUIRE(retained.has_value());
        auto pin = retained->reader.capture();
        REQUIRE(pin.has_value());
        retained_pin = std::move(pin.value());
        REQUIRE(registry.retire(retained->target).has_value());
    }
    CHECK(registry.resident_count() == resident_limit);

    auto rejected_source = make_source(&destroyed);
    auto* const rejected_pointer = rejected_source.get();
    const auto rejected = registry.admit(rejected_source);
    CHECK((!rejected.has_value() && rejected.error() == error::capacity_exceeded));
    CHECK(rejected_source.get() == rejected_pointer);

    std::array<std::size_t, resident_limit> release_order{};
    std::size_t next_index = 0U;
    for (auto& index : release_order) {
        index = next_index;
        ++next_index;
    }
    auto schedule_state = seed;
    for (std::size_t remaining = resident_limit; remaining > 1U; --remaining) {
        const auto selected =
            static_cast<std::size_t>(next_schedule_value(schedule_state) % remaining);
        std::swap(release_order[remaining - 1U], release_order[selected]);
    }
    for (const auto index : std::span{release_order}.first(recovery_count)) {
        retained_pins[index].release();
    }

    auto recovered_source = make_source(&destroyed);
    auto recovered = registry.admit(recovered_source);
    REQUIRE(recovered.has_value());
    CHECK(!recovered_source);
    CHECK(registry.live_count() == 1U);
    CHECK(registry.resident_count() == resident_limit - recovery_count + 1U);
    CHECK(destroyed.call_count() == recovery_count);

    for (auto& retained_pin : retained_pins) {
        retained_pin.release();
    }
    CHECK(registry.drain_retired() == resident_limit - recovery_count);
    CHECK(registry.resident_count() == 1U);
    CHECK(destroyed.call_count() == resident_limit);
    REQUIRE(registry.retire(recovered->target).has_value());
    CHECK(registry.drain_retired() == 1U);
    CHECK(registry.resident_count() == 0U);
    CHECK(destroyed.call_count() == resident_limit + 1U);

    for (std::uint64_t reconnect = 0U; reconnect < reconnect_count; ++reconnect) {
        auto reconnect_source = make_source();
        auto current = registry.admit(reconnect_source);
        REQUIRE(current.has_value());
        REQUIRE(current->producer.publish(make_update(1U)).has_value());
        const auto snapshot = current->reader.read();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->target == current->target);
        CHECK(coherent(snapshot.value()));

        const auto stale_reader = current->reader;
        const auto stale_producer = current->producer;
        REQUIRE(registry.retire(current->target).has_value());
        const auto stale_capture = stale_reader.capture();
        const auto stale_publication = stale_producer.publish(make_update(1U));
        CHECK((!stale_capture.has_value() && stale_capture.error() == error::not_connected));
        CHECK(
            (!stale_publication.has_value() && stale_publication.error() == error::not_connected));
        CHECK(registry.drain_retired() == 1U);
        CHECK(registry.live_count() == 0U);
        CHECK(registry.resident_count() == 0U);
    }
}

} // namespace
