#include "gameinput_probe/report.hpp"

#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

[[nodiscard]] native_guid sequential_guid() {
    native_guid value{};
    for (std::size_t index = 0U; index < value.bytes.size(); ++index) {
        value.bytes[index] = static_cast<std::byte>(index);
    }
    return value;
}

TEST_CASE("gameinput.probe.report_shows_escaped_endpoint_and_container_identity",
    "[unit][gameinput][probe]") {
    metadata_snapshot snapshot{};
    snapshot.target.device.value = 7U;
    snapshot.container_id = {.availability = metadata_availability::available,
        .value = sequential_guid()};
    snapshot.container_id_source = container_id_provenance::exact_pnp_interface;
    snapshot.headset_render.id.availability = metadata_availability::available;
    snapshot.headset_render.id.value = u"Render-A";
    snapshot.headset_capture.id.availability = metadata_availability::available;
    snapshot.headset_capture.id.value = u"CaptureB";
    snapshot.native.haptic_endpoint_id.availability = metadata_availability::available;
    snapshot.native.haptic_endpoint_id.value = u"Haptic\\\"path\n\u03A9\U0001F3AE";
    snapshot.capabilities.availability = metadata_availability::available;
    snapshot.capabilities.value = {.gamepad = true,
        .low_frequency_rumble = true,
        .high_frequency_rumble = false,
        .trigger_rumble = true};

    const auto report = format_metadata(snapshot);

    CHECK(
        report.contains("container=available container_guid=03020100-0504-0706-0809-0a0b0c0d0e0f"));
    CHECK(report.contains("container_source=exact_pnp_interface"));
    CHECK(report.contains("headset_render_id=u16\"Render-A\""));
    CHECK(report.contains("headset_capture_id=u16\"CaptureB\""));
    CHECK(report.contains(R"(haptic_endpoint_id=u16"Haptic\\\"path\u000A\u03A9\uD83C\uDFAE")"));
    CHECK(report.contains(
        "capabilities=available gamepad=true low_frequency_rumble=true high_frequency_rumble=false "
        "trigger_rumble=true"));
}

TEST_CASE("gameinput.probe.report_omits_values_for_unavailable_metadata",
    "[unit][gameinput][probe]") {
    metadata_snapshot snapshot{};
    snapshot.target.device.value = 8U;
    snapshot.container_id.availability = metadata_availability::unavailable;
    snapshot.container_id.value = sequential_guid();
    snapshot.headset_render.id.availability = metadata_availability::not_present;
    snapshot.headset_render.id.value = u"must-not-appear-render";
    snapshot.headset_capture.id.availability = metadata_availability::unavailable;
    snapshot.headset_capture.id.value = u"must-not-appear-capture";
    snapshot.native.haptic_endpoint_id.availability = metadata_availability::unsupported;
    snapshot.native.haptic_endpoint_id.value = u"must-not-appear-haptic";

    const auto report = format_metadata(snapshot);

    CHECK(report.contains("container=unavailable"));
    CHECK_FALSE(report.contains("container_guid="));
    CHECK(report.contains("headset_render=not_present"));
    CHECK(report.contains("headset_capture=unavailable"));
    CHECK(report.contains("haptic_endpoint=unsupported"));
    CHECK_FALSE(report.contains("_id="));
    CHECK_FALSE(report.contains("must-not-appear"));
}

TEST_CASE("gameinput.probe.report_bounds_oversized_endpoint_values", "[unit][gameinput][probe]") {
    metadata_snapshot snapshot{};
    snapshot.headset_render.id.availability = metadata_availability::available;
    snapshot.headset_render.id.value.assign(headset_endpoint_content_code_units + 1U, u'x');
    snapshot.native.haptic_endpoint_id.availability = metadata_availability::available;
    snapshot.native.haptic_endpoint_id.value.assign(haptic_endpoint_content_code_units + 1U, u'y');

    const auto report = format_metadata(snapshot);

    CHECK(report.contains("headset_render_id=over_limit:32768_code_units"));
    CHECK(report.contains("haptic_endpoint_id=over_limit:256_code_units"));
    CHECK(report.size() < 2048U);
}

TEST_CASE("gameinput.probe.report_requirement_status_respects_requested_checks",
    "[unit][gameinput][probe]") {
    CHECK(format_requirements({}).line == "requirement=met device=false reading=false");
    CHECK_FALSE(format_requirements({.device_required = true}).met);
    CHECK(format_requirements({.device_required = true}).line ==
          "requirement=not_met device=false reading=false");
    CHECK(format_requirements({.reading_required = true, .reading_observed = true}).met);
    CHECK(format_requirements({.reading_required = true, .reading_observed = true}).line ==
          "requirement=met device=false reading=true");
}

} // namespace
