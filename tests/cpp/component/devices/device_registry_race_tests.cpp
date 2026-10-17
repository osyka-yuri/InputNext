#include <inputnext/device_registry.hpp>

#include "test_start_gate.hpp"
#include "test_support.hpp"
#include <inputnext/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;

class worker_cleanup final {
public:
    worker_cleanup(start_gate& gate, std::array<std::jthread, 2U>& workers) noexcept
        : gate_(gate),
          workers_(workers) {}

    ~worker_cleanup() {
        gate_.release();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    worker_cleanup(const worker_cleanup&) = delete;
    worker_cleanup& operator=(const worker_cleanup&) = delete;
    worker_cleanup(worker_cleanup&&) = delete;
    worker_cleanup& operator=(worker_cleanup&&) = delete;

private:
    start_gate& gate_;
    std::array<std::jthread, 2U>& workers_;
};

TEST_CASE("devices.race.publish_retire", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto source = make_source(&destroyed);
    auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    auto retained_pin_result = admitted->reader.capture();
    REQUIRE(retained_pin_result.has_value());
    auto retained_pin = std::move(retained_pin_result.value());

    const auto producer = admitted->producer;
    const auto target = admitted->target;
    start_gate start;
    std::optional<result<std::uint64_t>> publication_result;
    std::optional<result<void>> retirement_result;
    bool publication_threw = false;
    bool retirement_threw = false;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{start, workers};

    workers[0U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            publication_result.emplace(producer.publish(make_update(1U)));
        } catch (...) {
            publication_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            retirement_result.emplace(registry.retire(target));
        } catch (...) {
            retirement_threw = true;
        }
    }};

    // The rendezvous starts both public calls together; it does not pause either inside the implementation.
    start.wait_for_ready(workers.size());
    start.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(!publication_threw);
    CHECK(!retirement_threw);
    CHECK(publication_result.has_value());
    if (publication_result.has_value()) {
        if (publication_result->has_value()) {
            CHECK(publication_result->value() == 1U);
        } else {
            CHECK(publication_result->error() == error::not_connected);
        }
    }
    CHECK(retirement_result.has_value());
    if (retirement_result.has_value()) {
        CHECK(retirement_result->has_value());
    }

    const auto stale_publication = producer.publish(make_update(2U));
    CHECK((!stale_publication.has_value() && stale_publication.error() == error::not_connected));
    const auto revoked_pin_read = retained_pin.read();
    CHECK((!revoked_pin_read.has_value() && revoked_pin_read.error() == error::not_connected));
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 1U);
    CHECK(destroyed.call_count() == 0U);

    retained_pin.release();
    CHECK(registry.drain_retired() == 1U);
    CHECK(registry.resident_count() == 0U);
    CHECK(destroyed.call_count() == 1U);
}

TEST_CASE("devices.race.publish_replace", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto original_source = make_source(&destroyed);
    auto original = registry.admit(original_source);
    REQUIRE(original.has_value());
    REQUIRE(original->producer.publish(make_update(1U)).value() == 1U);
    auto retained_pin_result = original->reader.capture();
    REQUIRE(retained_pin_result.has_value());
    auto retained_pin = std::move(retained_pin_result.value());

    auto replacement_source = make_source(&destroyed);
    const auto old_producer = original->producer;
    const auto old_target = original->target;
    start_gate start;
    std::optional<result<std::uint64_t>> publication_result;
    std::optional<result<admitted_connection>> replacement_result;
    bool publication_threw = false;
    bool replacement_threw = false;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{start, workers};

    workers[0U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            publication_result.emplace(old_producer.publish(make_update(2U)));
        } catch (...) {
            publication_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            replacement_result.emplace(registry.replace(old_target, replacement_source));
        } catch (...) {
            replacement_threw = true;
        }
    }};

    start.wait_for_ready(workers.size());
    start.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(!publication_threw);
    CHECK(!replacement_threw);
    CHECK(publication_result.has_value());
    if (publication_result.has_value()) {
        if (publication_result->has_value()) {
            CHECK(publication_result->value() == 2U);
        } else {
            CHECK(publication_result->error() == error::not_connected);
        }
    }
    CHECK(replacement_result.has_value());
    if (!replacement_result.has_value()) {
        return;
    }
    auto& replacement_operation = *replacement_result;
    CHECK(replacement_operation.has_value());
    if (!replacement_operation.has_value()) {
        return;
    }
    const auto replacement = replacement_operation.value();
    CHECK(!replacement_source);
    CHECK(replacement.target.runtime == original->target.runtime);
    CHECK(replacement.target.device == original->target.device);
    CHECK(replacement.target.connection != original->target.connection);
    CHECK(replacement.target != original->target);
    CHECK(registry.resident_count() == 2U);
    CHECK(destroyed.call_count() == 0U);

    const auto old_pin_read = retained_pin.read();
    CHECK((!old_pin_read.has_value() && old_pin_read.error() == error::not_connected));
    const auto stale_publication = original->producer.publish(make_update(3U));
    CHECK((!stale_publication.has_value() && stale_publication.error() == error::not_connected));
    const auto empty_replacement_read = replacement.reader.read();
    CHECK(
        (!empty_replacement_read.has_value() && empty_replacement_read.error() == error::no_data));

    const auto fresh_publication = replacement.producer.publish(make_update(1U));
    REQUIRE(fresh_publication.has_value());
    CHECK(fresh_publication.value() == 1U);
    const auto fresh_read = replacement.reader.read();
    REQUIRE(fresh_read.has_value());
    CHECK(fresh_read->target == replacement.target);
    CHECK(fresh_read->sample.revision == 1U);
    CHECK(coherent(fresh_read.value()));

    retained_pin.release();
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 1U);
    REQUIRE(registry.retire(replacement.target).has_value());
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 2U);
}

TEST_CASE("devices.race.capture_replace", "[component][devices][concurrency]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    auto original_source = make_source(&destroyed);
    auto original = registry.admit(original_source);
    REQUIRE(original.has_value());
    REQUIRE(original->producer.publish(make_update(1U)).has_value());
    auto retained_pin_result = original->reader.capture();
    REQUIRE(retained_pin_result.has_value());
    auto retained_pin = std::move(retained_pin_result.value());

    auto replacement_source = make_source(&destroyed);
    const auto old_reader = original->reader;
    const auto old_target = original->target;
    start_gate start;
    std::optional<result<read_pin>> capture_result;
    std::optional<result<admitted_connection>> replacement_result;
    bool capture_threw = false;
    bool replacement_threw = false;
    read_pin captured_pin;
    std::array<std::jthread, 2U> workers{};
    const worker_cleanup cleanup{start, workers};

    workers[0U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            capture_result.emplace(old_reader.capture());
        } catch (...) {
            capture_threw = true;
        }
    }};
    workers[1U] = std::jthread{[&] {
        start.arrive_and_wait();
        try {
            replacement_result.emplace(registry.replace(old_target, replacement_source));
        } catch (...) {
            replacement_threw = true;
        }
    }};

    start.wait_for_ready(workers.size());
    start.release();
    for (auto& worker : workers) {
        worker.join();
    }

    CHECK(!capture_threw);
    CHECK(!replacement_threw);
    CHECK(capture_result.has_value());
    if (!capture_result.has_value()) {
        return;
    }
    auto& capture_operation = *capture_result;
    CHECK((capture_operation.has_value() || capture_operation.error() == error::not_connected));
    if (capture_operation.has_value()) {
        captured_pin = std::move(*capture_operation);
        CHECK(captured_pin.target() == original->target);
        const auto captured_read = captured_pin.read();
        CHECK((!captured_read.has_value() && captured_read.error() == error::not_connected));
    }
    CHECK(replacement_result.has_value());
    if (!replacement_result.has_value()) {
        return;
    }
    auto& replacement_operation = *replacement_result;
    CHECK(replacement_operation.has_value());
    if (!replacement_operation.has_value()) {
        return;
    }
    const auto replacement = replacement_operation.value();
    CHECK(!replacement_source);
    CHECK(replacement.target.runtime == original->target.runtime);
    CHECK(replacement.target.device == original->target.device);
    CHECK(replacement.target.connection != original->target.connection);
    CHECK(registry.resident_count() == 2U);
    CHECK(destroyed.call_count() == 0U);

    const auto stale_capture = old_reader.capture();
    CHECK((!stale_capture.has_value() && stale_capture.error() == error::not_connected));
    const auto empty_replacement_read = replacement.reader.read();
    CHECK(
        (!empty_replacement_read.has_value() && empty_replacement_read.error() == error::no_data));
    const auto fresh_publication = replacement.producer.publish(make_update(1U));
    REQUIRE(fresh_publication.has_value());
    CHECK(fresh_publication.value() == 1U);
    const auto fresh_read = replacement.reader.read();
    REQUIRE(fresh_read.has_value());
    CHECK(fresh_read->target == replacement.target);
    CHECK(fresh_read->sample.revision == 1U);
    CHECK(coherent(fresh_read.value()));

    captured_pin.release();
    retained_pin.release();
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 1U);
    REQUIRE(registry.retire(replacement.target).has_value());
    CHECK(registry.drain_retired() == 1U);
    CHECK(destroyed.call_count() == 2U);
}

} // namespace
