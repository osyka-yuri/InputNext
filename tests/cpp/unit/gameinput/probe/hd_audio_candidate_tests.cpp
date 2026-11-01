#include "gameinput_probe/output/hd_audio/candidate.hpp"

#include "gameinput_probe/output/dualsense_usb_candidate_selector.hpp"
#include "output/playback_test_support.hpp"

#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] metadata_snapshot qualified_snapshot(const dualsense_usb_candidate& candidate) {
    native_guid container{};
    container.bytes.front() = std::byte{1U};
    return {.target = candidate.target,
        .catalogue_generation = candidate.catalogue_generation,
        .native = {.container_id = {.availability = metadata_availability::available,
                       .value = container},
            .haptic_endpoint_id = {.availability = metadata_availability::available,
                .value = u"haptic-endpoint"}},
        .container_id = {.availability = metadata_availability::available, .value = container},
        .container_id_source = container_id_provenance::gameinput_native,
        .model = {.availability = metadata_availability::available, .value = candidate.model},
        .transport = {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::native_bus_type}}};
}

TEST_CASE("gameinput.probe.hd_candidate_requires_exact_current_authoritative_usb_evidence",
    "[unit][gameinput][probe][audio]") {
    const dualsense_usb_candidate candidate{.target = test::output::playback_target(1U),
        .catalogue_generation = 3U,
        .model = metadata_device_model::dualsense};
    auto snapshot = qualified_snapshot(candidate);
    REQUIRE(qualify_hd_audio_binding(snapshot, candidate).has_value());

    SECTION("an enumerator also establishes USB") {
        snapshot.transport.value.provenance = transport_provenance::native_enumerator;
        CHECK(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
    SECTION("a path heuristic cannot establish USB") {
        snapshot.transport.value.provenance = transport_provenance::pnp_path_heuristic;
        CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
    SECTION("a different catalogue generation cannot reuse the candidate") {
        ++snapshot.catalogue_generation;
        CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
    SECTION("a different connection cannot reuse the candidate") {
        snapshot.target = test::output::playback_target(2U);
        CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
    SECTION("Bluetooth cannot reuse a USB candidate") {
        snapshot.transport.value.value = physical_transport::bluetooth;
        CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
    SECTION("conflicting native and resolved containers cannot match") {
        snapshot.native.container_id.value.bytes.front() = std::byte{2U};
        CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
    }
}

TEST_CASE("gameinput.probe.hd_candidate_rejects_incomplete_endpoint_identity",
    "[unit][gameinput][probe][audio]") {
    const dualsense_usb_candidate candidate{.target = test::output::playback_target(1U),
        .catalogue_generation = 3U,
        .model = metadata_device_model::dualsense_edge};
    auto snapshot = qualified_snapshot(candidate);

    SECTION("an embedded null cannot identify a native endpoint") {
        snapshot.native.haptic_endpoint_id.value = std::u16string{u"haptic\0endpoint", 15U};
    }
    SECTION("an overlong endpoint is rejected without truncation") {
        snapshot.native.haptic_endpoint_id.value.assign(haptic_endpoint_buffer_code_units, u'x');
    }
    SECTION("an unavailable container cannot establish a reconnect identity") {
        snapshot.container_id.availability = metadata_availability::unavailable;
    }
    CHECK_FALSE(qualify_hd_audio_binding(snapshot, candidate).has_value());
}

} // namespace
} // namespace inputnext::gameinput_detail
