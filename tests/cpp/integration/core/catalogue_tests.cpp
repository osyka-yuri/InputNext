#include "core_test_support.hpp"

#include "core/core.hpp"
#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "service_test_access.hpp"
#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <thread>

namespace inputnext::core_test_support {
inline constexpr std::array<std::uint8_t, 32U> known_fingerprint{0xFFU,
    0x55U,
    0xC9U,
    0x79U,
    0x76U,
    0xA8U,
    0x40U,
    0xB4U,
    0xCEU,
    0xD9U,
    0x64U,
    0xEDU,
    0x49U,
    0xE3U,
    0x79U,
    0x45U,
    0x94U,
    0xBAU,
    0x3FU,
    0x67U,
    0x52U,
    0x38U,
    0xB5U,
    0xFDU,
    0x25U,
    0xD2U,
    0x82U,
    0xB6U,
    0x0FU,
    0x70U,
    0xA1U,
    0x94U};

} // namespace inputnext::core_test_support

TEST_CASE("core.catalogue_projects_hardware_identity_and_revisions",
    "[integration][core][catalogue]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();

    auto first = fixture.core->enumerate();
    REQUIRE(first.has_value());
    REQUIRE(first->count == 1U);
    CHECK(first->revision == 1U);
    CHECK(first->entries[0U].identity == inputnext::core::identity_quality::platform);
    CHECK(
        first->entries[0U].hardware_fingerprint == inputnext::core_test_support::known_fingerprint);
    CHECK(first->entries[0U].output_channels == inputnext::core::rumble_channel);

    auto unchanged = fixture.core->enumerate();
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->revision == first->revision);

    fixture.set_model_transport(inputnext::metadata_device_model::dualsense,
        inputnext::physical_transport::usb,
        inputnext::transport_provenance::native_bus_type);
    auto usb = fixture.core->enumerate();
    REQUIRE(usb.has_value());
    CHECK(usb->revision == first->revision + 1U);
    CHECK(usb->entries[0U].output_channels == 0x0FU);
    CHECK(usb->entries[0U].transport == inputnext::physical_transport::usb);

    fixture.set_model_transport(inputnext::metadata_device_model::dualsense,
        inputnext::physical_transport::usb,
        inputnext::transport_provenance::pnp_path_heuristic);
    auto heuristic_usb = fixture.core->enumerate();
    REQUIRE(heuristic_usb.has_value());
    CHECK(heuristic_usb->entries[0U].output_channels == 0U);

    fixture.set_model_transport(inputnext::metadata_device_model::dualsense,
        inputnext::physical_transport::usb,
        inputnext::transport_provenance::native_bus_type,
        true);
    auto conflicting = fixture.core->enumerate();
    REQUIRE(conflicting.has_value());
    CHECK(conflicting->entries[0U].output_channels == 0U);
    CHECK(conflicting->revision == heuristic_usb->revision);
    const auto exact = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::exact_target,
            .target = target});
    REQUIRE(exact.has_value());
    CHECK(exact.value() == target);
}

TEST_CASE("core.catalogue_returns_unavailable_when_admission_closes_during_metadata_query",
    "[integration][core][catalogue][shutdown]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    fixture.gameinput.metadata->block_next_queries(1U);

    std::optional<inputnext::core::result<inputnext::core::device_catalogue>> enumerated;
    std::jthread enumeration([&] { enumerated = fixture.core->enumerate(); });
    inputnext::core_test_support::scoped_query_release release{*fixture.gameinput.metadata, 1U};
    REQUIRE(fixture.gameinput.metadata->wait_for_blocked(1U));

    const auto latest = fixture.core->read_latest(target);
    REQUIRE(latest.has_value());
    CHECK(latest->target == target);

    fixture.core->close_admission();
    release.release();
    enumeration.join();

    const auto& completed = inputnext::core_test_support::required_value(enumerated);
    REQUIRE(!completed.has_value());
    CHECK(completed.error() == inputnext::core::status::unavailable);
}

TEST_CASE("core.catalogue_retries_inventory_churn_without_publishing_partial_data",
    "[integration][core][catalogue][concurrency]") {
    inputnext::core_test_support::fixture fixture;
    static_cast<void>(fixture.connect());
    fixture.gameinput.metadata->block_next_queries(1U);

    std::optional<inputnext::core::result<inputnext::core::device_catalogue>> result;
    std::jthread enumeration([&] { result = fixture.core->enumerate(); });
    inputnext::core_test_support::scoped_query_release release{*fixture.gameinput.metadata, 1U};
    REQUIRE(fixture.gameinput.metadata->wait_for_blocked(1U));
    static_cast<void>(fixture.connect(1U));
    release.release();
    enumeration.join();

    const auto& completed = inputnext::core_test_support::required_value(result);
    REQUIRE(completed.has_value());
    CHECK(completed->count == 2U);
    CHECK(completed->revision == 1U);
}

TEST_CASE("core.catalogue_marks_duplicate_fingerprints_ambiguous",
    "[integration][core][catalogue]") {
    inputnext::core_test_support::fixture fixture;
    static_cast<void>(fixture.connect());
    static_cast<void>(fixture.connect(1U));

    const auto catalogue = fixture.core->enumerate();
    REQUIRE(catalogue.has_value());
    REQUIRE(catalogue->count == 2U);
    for (const auto& entry : std::span{catalogue->entries}.first(catalogue->count)) {
        CHECK(entry.identity == inputnext::core::identity_quality::ambiguous);
    }

    const auto resolved = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::hardware_fingerprint,
            .hardware_fingerprint = inputnext::core_test_support::known_fingerprint});
    REQUIRE(!resolved.has_value());
    CHECK(resolved.error() == inputnext::core::status::ambiguous_target);
}

TEST_CASE("core.catalogue_resolves_a_unique_known_hardware_fingerprint",
    "[integration][core][selectors]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();

    const auto resolved = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::hardware_fingerprint,
            .hardware_fingerprint = inputnext::core_test_support::known_fingerprint});
    REQUIRE(resolved.has_value());
    CHECK(resolved.value() == target);
}

TEST_CASE("core.catalogue_revision_exhaustion_does_not_wrap_or_publish_changes",
    "[integration][core][catalogue][revision]") {
    inputnext::core_test_support::fixture fixture;
    static_cast<void>(fixture.connect());
    const auto initial = fixture.core->enumerate();
    REQUIRE(initial.has_value());

    inputnext::core::detail::service_test_access::set_catalogue_revision(*fixture.core,
        std::numeric_limits<std::uint64_t>::max());
    fixture.set_model_transport(inputnext::metadata_device_model::generic_gamepad,
        inputnext::physical_transport::bluetooth,
        inputnext::transport_provenance::native_bus_type);

    const auto exhausted = fixture.core->enumerate();
    REQUIRE(!exhausted.has_value());
    CHECK(exhausted.error() == inputnext::core::status::unavailable);
    CHECK(fixture.core->enumerate().error() == inputnext::core::status::unavailable);
}

TEST_CASE("core.catalogue_resolves_exact_and_xinput_selectors", "[integration][core][selectors]") {
    inputnext::core_test_support::fixture fixture;
    const auto target = fixture.connect();
    fixture.assign_xinput(target);

    const auto exact = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::exact_target,
            .target = target});
    REQUIRE(exact.has_value());
    CHECK(exact.value() == target);

    const auto slot = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::xinput_slot, .slot = 0U});
    REQUIRE(slot.has_value());
    CHECK(slot.value() == target);

    const auto invalid = fixture.core->resolve(
        inputnext::core::selector{.kind = inputnext::core::selector_kind::xinput_slot,
            .slot = 0U,
            .target = target});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == inputnext::core::status::invalid_argument);
}
