#include <inputnext/device_registry.hpp>

#include "test_support.hpp"
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;

constexpr std::size_t live_limit = 16U;
constexpr std::size_t resident_limit = 128U;

TEST_CASE("devices.exact_runtime_and_connection", "[component][devices]") {
    const auto runtime = make_runtime(std::byte{0xA5});
    device_registry registry{runtime};
    auto first_source = make_source();

    auto first = registry.admit(first_source);

    REQUIRE(first.has_value());
    CHECK(!(first_source));
    CHECK(first->target.valid());
    CHECK(first->target.runtime == runtime);
    CHECK(first->target.device.value == 1U);
    CHECK(first->target.connection.value == 1U);
    const auto initial_read = first->reader.read();
    CHECK((!initial_read.has_value() && initial_read.error() == error::no_data));

    auto replacement_source = make_source();
    auto replacement = registry.replace(first->target, replacement_source);

    REQUIRE(replacement.has_value());
    CHECK(!(replacement_source));
    CHECK(replacement->target.runtime == first->target.runtime);
    CHECK(replacement->target.device == first->target.device);
    CHECK(replacement->target.connection.value == 2U);
    CHECK(replacement->target != first->target);
    const auto stale_read = first->reader.read();
    CHECK((!stale_read.has_value() && stale_read.error() == error::not_connected));

    const auto update = make_update(1U);
    const auto publication = replacement->producer.publish(update);
    REQUIRE(publication.has_value());
    CHECK(publication.value() == 1U);
    const auto current = registry.read(replacement->target);
    REQUIRE(current.has_value());
    CHECK(current->target == replacement->target);
    CHECK(current->sample.state == update.state);
    CHECK(current->sample.source_timestamp_us == update.source_timestamp_us);
    CHECK(current->sample.observation_qpc == update.observation_qpc);
    CHECK(current->sample.revision == 1U);
}

TEST_CASE("devices.stale_and_foreign_targets", "[component][devices]") {
    device_registry registry{make_runtime()};
    const read_binding empty_reader;
    const auto empty_capture = empty_reader.capture();
    const auto empty_read = empty_reader.read();
    const producer_binding empty_producer;
    const auto empty_publish = empty_producer.publish(make_update(1U));
    read_pin empty_pin;
    CHECK((!empty_capture.has_value() && empty_capture.error() == error::invalid_argument));
    CHECK((!empty_read.has_value() && empty_read.error() == error::invalid_argument));
    CHECK((!empty_publish.has_value() && empty_publish.error() == error::invalid_argument));
    CHECK(!(empty_pin.target().valid()));
    const auto empty_pin_read = empty_pin.read();
    CHECK((!empty_pin_read.has_value() && empty_pin_read.error() == error::not_connected));
    empty_pin.release();

    auto source = make_source();
    const auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());

    auto foreign = admitted->target;
    foreign.runtime = make_runtime(std::byte{2});
    const auto foreign_read = registry.read(foreign);
    const auto foreign_retire = registry.retire(foreign);
    auto foreign_source = make_source();
    const auto foreign_replace = registry.replace(foreign, foreign_source);
    CHECK((!foreign_read.has_value() && foreign_read.error() == error::not_connected));
    CHECK((!foreign_retire.has_value() && foreign_retire.error() == error::not_connected));
    CHECK((!foreign_replace.has_value() && foreign_replace.error() == error::not_connected));
    CHECK(foreign_source != nullptr);

    const auto zero_target = registry.read(connection_target{});
    CHECK((!zero_target.has_value() && zero_target.error() == error::invalid_argument));

    REQUIRE(registry.retire(admitted->target).has_value());
    const auto retired_read = registry.read(admitted->target);
    const auto retired_capture = registry.capture(admitted->target);
    CHECK((!retired_read.has_value() && retired_read.error() == error::not_connected));
    CHECK((!retired_capture.has_value() && retired_capture.error() == error::not_connected));
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 1U);
    CHECK(registry.drain_retired() == 1U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.validate_capture_checks_admission_without_extending_residency",
    "[component][devices]") {
    device_registry registry{make_runtime()};
    const read_binding empty_reader;
    const auto empty_validation = empty_reader.validate_capture();
    CHECK((!empty_validation.has_value() && empty_validation.error() == error::invalid_argument));

    auto source = make_source();
    const auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    CHECK(admitted->reader.validate_capture().has_value());

    auto pin = admitted->reader.capture();
    REQUIRE(pin.has_value());
    CHECK(admitted->reader.validate_capture().has_value());
    REQUIRE(registry.retire(admitted->target).has_value());
    const auto retired_validation = admitted->reader.validate_capture();
    CHECK((!retired_validation.has_value() && retired_validation.error() == error::not_connected));

    pin->release();
    CHECK(registry.drain_retired() == 1U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.failed_admission_is_atomic", "[component][devices]") {
    device_registry registry{make_runtime()};
    std::unique_ptr<source_lifetime> empty_source;
    const auto empty_admission = registry.admit(empty_source);
    CHECK((!empty_admission.has_value() && empty_admission.error() == error::invalid_argument));

    auto current_source = make_source();
    auto current = registry.admit(current_source);
    REQUIRE(current.has_value());

    auto invalid_replacement_source = make_source();
    auto* const invalid_replacement_pointer = invalid_replacement_source.get();
    const auto invalid_replacement =
        registry.replace(connection_target{}, invalid_replacement_source);
    CHECK((!invalid_replacement.has_value() &&
           invalid_replacement.error() == error::invalid_argument));
    CHECK(invalid_replacement_source.get() == invalid_replacement_pointer);

    std::array<read_pin, resident_limit - 1U> retained_pins{};
    for (auto& retained_pin : retained_pins) {
        auto retired_source = make_source();
        auto retired = registry.admit(retired_source);
        REQUIRE(retired.has_value());
        auto pin = retired->reader.capture();
        REQUIRE(pin.has_value());
        retained_pin = std::move(pin.value());
        REQUIRE(registry.retire(retired->target).has_value());
    }
    REQUIRE(registry.live_count() == 1U);
    REQUIRE(registry.resident_count() == resident_limit);

    auto replacement_source = make_source();
    auto* const replacement_pointer = replacement_source.get();
    const auto rejected = registry.replace(current->target, replacement_source);
    REQUIRE(!rejected.has_value());
    CHECK(rejected.error() == error::capacity_exceeded);
    CHECK(replacement_source.get() == replacement_pointer);
    CHECK(registry.live_count() == 1U);
    CHECK(registry.resident_count() == resident_limit);

    const auto original_publication = current->producer.publish(make_update(1U));
    REQUIRE(original_publication.has_value());
    CHECK(original_publication.value() == 1U);
    const auto original_read = current->reader.read();
    REQUIRE(original_read.has_value());
    CHECK(original_read->target == current->target);
    CHECK(original_read->sample.revision == 1U);

    retained_pins.front().release();
    const auto recovered = registry.replace(current->target, replacement_source);
    REQUIRE(recovered.has_value());
    CHECK(!(replacement_source));
    CHECK(recovered->target.device == current->target.device);
    CHECK(recovered->target.connection.value == static_cast<std::uint64_t>(resident_limit + 1U));
    CHECK(registry.live_count() == 1U);
    CHECK(registry.resident_count() == resident_limit);
    const auto stale_publication = current->producer.publish(make_update(2U));
    CHECK((!stale_publication.has_value() && stale_publication.error() == error::not_connected));
    const auto replacement_read = recovered->reader.read();
    CHECK((!replacement_read.has_value() && replacement_read.error() == error::no_data));

    registry.retire_all();
    for (auto& pin : retained_pins) {
        pin.release();
    }
    CHECK(registry.drain_retired() == resident_limit - 2U);
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.live_limit", "[component][devices]") {
    device_registry registry{make_runtime()};
    std::array<admitted_connection, live_limit> live{};

    for (auto& connection : live) {
        auto source = make_source();
        auto admitted = registry.admit(source);
        REQUIRE(admitted.has_value());
        CHECK(!(source));
        connection = admitted.value();
    }

    CHECK(registry.live_count() == live_limit);
    CHECK(registry.resident_count() == live_limit);

    auto rejected_source = make_source();
    auto* const rejected_pointer = rejected_source.get();
    const auto rejected = registry.admit(rejected_source);
    REQUIRE(!rejected.has_value());
    CHECK(rejected.error() == error::capacity_exceeded);
    CHECK(rejected_source.get() == rejected_pointer);
    CHECK(registry.live_count() == live_limit);
    CHECK(registry.resident_count() == live_limit);

    registry.retire_all();
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.retained_residence_limit", "[component][devices]") {
    device_registry registry{make_runtime()};
    std::array<read_pin, resident_limit> retained_pins{};

    for (std::size_t index = 0U; index < resident_limit; ++index) {
        auto source = make_source();
        auto admitted = registry.admit(source);
        REQUIRE(admitted.has_value());
        auto pin = admitted->reader.capture();
        REQUIRE(pin.has_value());
        retained_pins[index] = std::move(pin.value());
        REQUIRE(registry.retire(admitted->target).has_value());
    }

    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == resident_limit);
    auto rejected_source = make_source();
    auto* const rejected_pointer = rejected_source.get();
    const auto rejected = registry.admit(rejected_source);
    REQUIRE(!rejected.has_value());
    CHECK(rejected.error() == error::capacity_exceeded);
    CHECK(rejected_source.get() == rejected_pointer);
    CHECK(registry.resident_count() == resident_limit);

    for (auto& pin : retained_pins) {
        pin.release();
    }
    CHECK(registry.drain_retired() == resident_limit);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.direct_admission_recovers_capacity", "[component][devices]") {
    destruction_observer destroyed;
    device_registry registry{make_runtime()};
    std::array<read_pin, resident_limit> retained_pins{};

    for (std::size_t index = 0U; index < resident_limit; ++index) {
        auto source = make_source(&destroyed);
        auto admitted = registry.admit(source);
        REQUIRE(admitted.has_value());
        auto pin = admitted->reader.capture();
        REQUIRE(pin.has_value());
        retained_pins[index] = std::move(pin.value());
        REQUIRE(registry.retire(admitted->target).has_value());
    }

    CHECK(registry.resident_count() == resident_limit);
    for (std::size_t index = 0U; index < 3U; ++index) {
        retained_pins[index].release();
    }
    CHECK(destroyed.call_count() == 0U);

    auto replacement_source = make_source(&destroyed);
    const auto admitted = registry.admit(replacement_source);
    REQUIRE(admitted.has_value());
    CHECK(!(replacement_source));
    CHECK(registry.live_count() == 1U);
    CHECK(registry.resident_count() == resident_limit - 2U);
    CHECK(destroyed.call_count() == 3U);
    CHECK(retained_pins[3U].target().valid());

    registry.retire_all();
    for (std::size_t index = 3U; index < retained_pins.size(); ++index) {
        retained_pins[index].release();
    }
    CHECK(registry.drain_retired() == resident_limit - 3U);
    CHECK(registry.live_count() == 0U);
    CHECK(registry.resident_count() == 0U);
    CHECK(destroyed.call_count() == resident_limit + 1U);
}

TEST_CASE("devices.identity_exhaustion", "[component][devices]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto runtime = make_runtime();

    SECTION("final connection identity is used once") {
        device_registry registry{runtime,
            identity_seed{.next_device = 1U, .next_connection = maximum}};
        auto source = make_source();
        auto admitted = registry.admit(source);
        REQUIRE(admitted.has_value());
        CHECK(admitted->target.connection.value == maximum);

        auto replacement_source = make_source();
        auto* const replacement_pointer = replacement_source.get();
        const auto rejected_replace = registry.replace(admitted->target, replacement_source);
        REQUIRE(!rejected_replace.has_value());
        CHECK(rejected_replace.error() == error::identity_exhausted);
        CHECK(replacement_source.get() == replacement_pointer);
        const auto still_live = admitted->reader.read();
        CHECK((!still_live.has_value() && still_live.error() == error::no_data));

        registry.retire_all();
        const auto rejected_after_retire_all = registry.admit(replacement_source);
        CHECK((!rejected_after_retire_all.has_value() &&
               rejected_after_retire_all.error() == error::identity_exhausted));
        CHECK(replacement_source.get() == replacement_pointer);
    }

    SECTION("final device identity is used once") {
        device_registry registry{runtime,
            identity_seed{.next_device = maximum, .next_connection = 1U}};
        auto source = make_source();
        auto admitted = registry.admit(source);
        REQUIRE(admitted.has_value());
        CHECK(admitted->target.device.value == maximum);
        CHECK(admitted->target.connection.value == 1U);

        auto replacement_source = make_source();
        auto replacement = registry.replace(admitted->target, replacement_source);
        REQUIRE(replacement.has_value());
        CHECK(replacement->target.device.value == maximum);
        CHECK(replacement->target.connection.value == 2U);
        CHECK(replacement->target != admitted->target);

        registry.retire_all();
        auto next_source = make_source();
        auto* const next_pointer = next_source.get();
        const auto rejected = registry.admit(next_source);
        CHECK((!rejected.has_value() && rejected.error() == error::identity_exhausted));
        CHECK(next_source.get() == next_pointer);
    }

    SECTION("zero runtime and seeds are rejected") {
        CHECK_THROWS_AS(device_registry{runtime_id{}}, std::invalid_argument);
        CHECK_THROWS_AS(
            (device_registry{runtime, identity_seed{.next_device = 0U, .next_connection = 1U}}),
            std::invalid_argument);
        CHECK_THROWS_AS(
            (device_registry{runtime, identity_seed{.next_device = 1U, .next_connection = 0U}}),
            std::invalid_argument);
    }
}

} // namespace
