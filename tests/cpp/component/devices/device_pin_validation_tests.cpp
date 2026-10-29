#include <inputnext/device_registry.hpp>

#include "test_support.hpp"
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::test::devices;

TEST_CASE("devices.pin_validation_follows_ownership_and_closure", "[component][devices]") {
    const read_pin empty;
    const auto empty_status = empty.validate_live();
    CHECK((!empty_status.has_value() && empty_status.error() == error::not_connected));

    device_registry registry{make_runtime()};
    auto source = make_source();
    const auto admitted = registry.admit(source);
    REQUIRE(admitted.has_value());
    auto captured = admitted->reader.capture();
    REQUIRE(captured.has_value());
    auto pin = std::move(*captured);
    CHECK(pin.validate_live().has_value());
    const auto unpublished = pin.read();
    CHECK((!unpublished.has_value() && unpublished.error() == error::no_data));

    auto moved = std::move(pin);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): Moved-from pins are empty handles.
    CHECK(!pin.validate_live().has_value());
    CHECK(moved.validate_live().has_value());
    REQUIRE(admitted->producer.publish(make_update(1U)).has_value());
    CHECK(moved.read().has_value());

    read_pin assigned;
    assigned = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): Moved-from pins are empty handles.
    CHECK(!moved.validate_live().has_value());
    CHECK(assigned.validate_live().has_value());
    REQUIRE(registry.retire(admitted->target).has_value());
    const auto retired = assigned.validate_live();
    CHECK((!retired.has_value() && retired.error() == error::not_connected));
    CHECK(registry.drain_retired() == 0U);
    CHECK(registry.resident_count() == 1U);

    assigned.release();
    CHECK(!assigned.validate_live().has_value());
    CHECK(registry.drain_retired() == 1U);
    CHECK(registry.resident_count() == 0U);
}

TEST_CASE("devices.pin_validation_does_not_follow_a_replacement", "[component][devices]") {
    device_registry registry{make_runtime()};
    auto source = make_source();
    const auto first = registry.admit(source);
    REQUIRE(first.has_value());
    auto original = first->reader.capture();
    REQUIRE(original.has_value());

    auto replacement_source = make_source();
    const auto replacement = registry.replace(first->target, replacement_source);
    REQUIRE(replacement.has_value());
    auto current = replacement->reader.capture();
    REQUIRE(current.has_value());
    CHECK(!original->validate_live().has_value());
    CHECK(original->target() == first->target);
    CHECK(current->validate_live().has_value());
    CHECK(current->target() == replacement->target);
    CHECK(registry.drain_retired() == 0U);

    original->release();
    CHECK(registry.drain_retired() == 1U);
    auto next_source = make_source();
    const auto next = registry.admit(next_source);
    REQUIRE(next.has_value());
    CHECK(!first->reader.capture().has_value());
    CHECK(!original->validate_live().has_value());
    CHECK(current->validate_live().has_value());
}

} // namespace
