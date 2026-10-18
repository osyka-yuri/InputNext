#include "metadata/windows/os_port.hpp"

#include "metadata/provider.hpp"
#include "metadata/windows/provider.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace inputnext;
using namespace inputnext::metadata_detail;

[[nodiscard]] native_guid make_guid(const std::uint8_t value) {
    native_guid result{};
    result.bytes[0U] = static_cast<std::byte>(value);
    result.bytes[15U] = static_cast<std::byte>(value ^ 0xA5U);
    return result;
}

[[nodiscard]] connection_target make_target(const std::uint8_t value = 1U) {
    connection_target target{};
    target.runtime.bytes[0U] = static_cast<std::byte>(value);
    target.device.value = value;
    target.connection.value = value;
    return target;
}

[[nodiscard]] metadata_request make_request(const native_guid container = make_guid(1U)) {
    metadata_request request{};
    request.target = make_target();
    request.catalogue_generation = 17U;
    request.facts.vendor_id = {.availability = metadata_availability::available,
        .value = 0x1234U,
        .native_code = 0U};
    request.facts.product_id = {.availability = metadata_availability::available,
        .value = 0x5678U,
        .native_code = 0U};
    request.facts.revision = {.availability = metadata_availability::available,
        .value = 3U,
        .native_code = 0U};
    request.facts.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    request.facts.pnp_path_utf8 = {.availability = metadata_availability::available,
        .value = R"(\\?\HID#device-usb)",
        .native_code = 0U};
    request.facts.gameinput_capabilities = {.availability = metadata_availability::available,
        .value = native_gameinput_capabilities{.gamepad = true},
        .native_code = 0U};
    request.facts.haptic_endpoint_id = {.availability = metadata_availability::not_present,
        .value = {},
        .native_code = 0U};
    return request;
}

[[nodiscard]] audio_endpoint_observation make_endpoint(const native_guid container,
    const std::u16string_view id,
    const metadata_field<native_guid> direct_sound = {}) {
    return {
        .container_id = {.availability = metadata_availability::available,
            .value = container,
            .native_code = 0U},
        .endpoint_id = {.availability = metadata_availability::available,
            .value = std::u16string{id},
            .native_code = 0U},
        .direct_sound_guid = direct_sound,
    };
}

class scripted_windows_os final : public windows_metadata_os {
public:
    exact_pnp_observation pnp{};
    std::vector<audio_endpoint_observation> render_endpoints;
    std::vector<audio_endpoint_observation> capture_endpoints;
    audio_scan_result render_result{.availability = metadata_availability::available,
        .native_code = 0U};
    audio_scan_result capture_result{.availability = metadata_availability::available,
        .native_code = 0U};
    std::size_t pnp_calls{};
    std::size_t render_calls{};
    std::size_t capture_calls{};
    std::string last_path;
    native_guid last_render_container{};
    native_guid last_capture_container{};

    [[nodiscard]] exact_pnp_observation inspect_exact_pnp_interface(
        const std::string_view path) override {
        ++pnp_calls;
        last_path.assign(path);
        return pnp;
    }

    [[nodiscard]] audio_scan_result scan_active_audio(const audio_flow flow,
        const native_guid& container,
        const audio_endpoint_visitor visitor,
        void* const context) override {
        auto const& endpoints = flow == audio_flow::render ? render_endpoints : capture_endpoints;
        if (flow == audio_flow::render) {
            ++render_calls;
            last_render_container = container;
        } else {
            ++capture_calls;
            last_capture_container = container;
        }
        for (const auto& endpoint : endpoints) {
            visitor(context, endpoint);
        }
        return flow == audio_flow::render ? render_result : capture_result;
    }
};

TEST_CASE("metadata.provider_matches_exact_container_and_returns_owned_snapshot",
    "[component][metadata]") {
    const auto target_container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = target_container,
        .native_code = 0U};
    os.pnp.transport = {.availability = metadata_availability::available,
        .value = {.value = physical_transport::usb,
            .provenance = transport_provenance::native_bus_type},
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(make_guid(2U), u"unrelated-device-headset"));
    os.render_endpoints.push_back(make_endpoint(target_container, u"render-endpoint"));
    os.capture_endpoints.push_back(make_endpoint(target_container, u"capture-endpoint"));

    const auto provider = make_windows_metadata_provider(os);
    auto request = make_request(target_container);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    CHECK(result->target == request.target);
    CHECK(result->catalogue_generation == request.catalogue_generation);
    CHECK(result->container_id.has_value());
    CHECK(result->container_id.value == target_container);
    CHECK(result->container_id_source == container_id_provenance::gameinput_native);
    CHECK(result->transport.has_value());
    CHECK(result->transport.value.value == physical_transport::usb);
    CHECK(result->transport.value.provenance == transport_provenance::native_bus_type);
    REQUIRE(result->headset_render.id.has_value());
    CHECK(result->headset_render.id.value == u"render-endpoint");
    REQUIRE(result->headset_capture.id.has_value());
    CHECK(result->headset_capture.id.value == u"capture-endpoint");
    CHECK(result->headset_render.direct_sound_guid.availability ==
          metadata_availability::unavailable);
    CHECK(result->battery_telemetry.availability == metadata_availability::unsupported);
    CHECK(os.last_render_container == target_container);
    CHECK(os.last_capture_container == target_container);
    CHECK(os.last_path == request.facts.pnp_path_utf8.value);
}

TEST_CASE("metadata.provider_keeps_same_vid_pid_devices_separate_by_exact_container",
    "[component][metadata]") {
    const auto first_container = make_guid(1U);
    const auto second_container = make_guid(2U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = first_container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(first_container, u"first-device-headset"));
    os.render_endpoints.push_back(make_endpoint(second_container, u"second-device-headset"));
    const auto first_request = make_request(first_container);

    const auto provider = make_windows_metadata_provider(os);
    auto first = provider->query(first_request);
    REQUIRE(first.has_value());
    REQUIRE(first->headset_render.id.has_value());
    CHECK(first->headset_render.id.value == u"first-device-headset");

    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = second_container,
        .native_code = 0U};
    auto second_request = make_request(second_container);
    second_request.target = make_target(2U);
    auto second = provider->query(second_request);

    REQUIRE(second.has_value());
    REQUIRE(second->headset_render.id.has_value());
    CHECK(second->headset_render.id.value == u"second-device-headset");
    CHECK(first->target != second->target);
    CHECK(first->container_id.value != second->container_id.value);
}

TEST_CASE("metadata.provider_recovers_missing_container_from_exact_pnp", "[component][metadata]") {
    const auto recovered_container = make_guid(3U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = recovered_container,
        .native_code = 0U};
    os.pnp.transport = {.availability = metadata_availability::unavailable,
        .value = {.value = physical_transport::unknown,
            .provenance = transport_provenance::unavailable},
        .native_code = 0U};
    auto request = make_request();
    request.facts.container_id = {.availability = metadata_availability::not_present,
        .value = {},
        .native_code = 0U};
    request.facts.pnp_path_utf8.value = R"(\\?\BTHENUM#controller)";

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    CHECK(result->container_id.has_value());
    CHECK(result->container_id.value == recovered_container);
    CHECK(result->container_id_source == container_id_provenance::exact_pnp_interface);
    CHECK(result->transport.has_value());
    CHECK(result->transport.value.value == physical_transport::bluetooth);
    CHECK(result->transport.value.provenance == transport_provenance::pnp_path_heuristic);
}

TEST_CASE("metadata.provider_fails_closed_on_conflicting_exact_containers",
    "[component][metadata]") {
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = make_guid(2U),
        .native_code = 0U};
    const auto request = make_request(make_guid(1U));

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    CHECK(result->container_id.availability == metadata_availability::ambiguous);
    CHECK(result->container_id_source == container_id_provenance::conflicting);
    CHECK(result->headset_render.id.availability == metadata_availability::ambiguous);
    CHECK(result->headset_capture.id.availability == metadata_availability::ambiguous);
    CHECK(os.render_calls == 0U);
    CHECK(os.capture_calls == 0U);
}

TEST_CASE("metadata.provider_reports_distinct_endpoint_ambiguity_and_collapses_duplicates",
    "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(container, u"render-a"));
    os.render_endpoints.push_back(make_endpoint(container, u"render-b"));
    os.capture_endpoints.push_back(make_endpoint(container, u"capture-one"));
    os.capture_endpoints.push_back(make_endpoint(container, u"CAPTURE-ONE"));

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(make_request(container));

    REQUIRE(result.has_value());
    CHECK(result->headset_render.id.availability == metadata_availability::ambiguous);
    REQUIRE(result->headset_capture.id.has_value());
    CHECK(result->headset_capture.id.value == u"capture-one");
}

TEST_CASE("metadata.provider_excludes_known_generic_haptic_endpoint", "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(container, u"HAPTIC-ENDPOINT"));
    os.render_endpoints.push_back(make_endpoint(container, u"headset-endpoint"));
    auto request = make_request(container);
    request.facts.haptic_endpoint_id = {.availability = metadata_availability::available,
        .value = u"haptic-endpoint",
        .native_code = 0U};

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    REQUIRE(result->headset_render.id.has_value());
    CHECK(result->headset_render.id.value == u"headset-endpoint");
    CHECK(result->native.haptic_endpoint_id.value == u"haptic-endpoint");
}

TEST_CASE("metadata.provider_excludes_dualsense_and_edge_audio_from_headsets",
    "[component][metadata]") {
    for (const auto product : {0x0CE6U, 0x0DF2U}) {
        for (const bool has_container : {true, false}) {
            scripted_windows_os os{};
            auto request = make_request();
            request.facts.vendor_id.value = 0x054CU;
            request.facts.product_id.value = static_cast<std::uint16_t>(product);
            if (has_container) {
                os.pnp.container_id = request.facts.container_id;
                os.render_endpoints.push_back(
                    make_endpoint(request.facts.container_id.value, u"haptic-endpoint"));
            } else {
                request.facts.container_id = {.availability = metadata_availability::not_present,
                    .value = {},
                    .native_code = 0U};
                request.facts.pnp_path_utf8 = {.availability = metadata_availability::not_present,
                    .value = {},
                    .native_code = 0U};
            }

            const auto provider = make_windows_metadata_provider(os);
            const auto result = provider->query(request);

            REQUIRE(result.has_value());
            CHECK(result->headset_render.id.availability == metadata_availability::not_present);
            CHECK(result->headset_capture.id.availability == metadata_availability::not_present);
            CHECK(os.render_calls == 0U);
            CHECK(os.capture_calls == 0U);
        }
    }
}

TEST_CASE("metadata.provider_preserves_independent_partial_audio_results",
    "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(container, u"render-endpoint"));
    os.capture_endpoints.push_back(make_endpoint(container, u"capture-endpoint"));
    os.render_result = {.availability = metadata_availability::unavailable,
        .native_code = 0x80004005U};

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(make_request(container));

    REQUIRE(result.has_value());
    CHECK(result->headset_render.id.availability == metadata_availability::unavailable);
    REQUIRE(result->headset_capture.id.has_value());
    CHECK(result->headset_capture.id.value == u"capture-endpoint");
}

TEST_CASE("metadata.provider_never_claims_uniqueness_after_endpoint_capacity",
    "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(container, u"one-candidate"));
    os.render_result = {.availability = metadata_availability::capacity_exceeded,
        .native_code = 0U};

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(make_request(container));

    REQUIRE(result.has_value());
    CHECK(result->headset_render.id.availability == metadata_availability::capacity_exceeded);
}

TEST_CASE("metadata.provider_copies_results_across_later_queries", "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(make_endpoint(container, u"first-snapshot"));
    const auto provider = make_windows_metadata_provider(os);

    auto first = provider->query(make_request(container));
    REQUIRE(first.has_value());
    REQUIRE(first->headset_render.id.has_value());
    os.render_endpoints.front().endpoint_id.value = u"second-snapshot";
    auto second = provider->query(make_request(container));

    REQUIRE(second.has_value());
    REQUIRE(second->headset_render.id.has_value());
    CHECK(first->headset_render.id.value == u"first-snapshot");
    CHECK(second->headset_render.id.value == u"second-snapshot");
}

TEST_CASE("metadata.provider_bounds_native_strings_before_os_calls", "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    auto request = make_request(container);
    request.facts.pnp_path_utf8.value.assign(max_pnp_path_utf8_bytes + 1U, 'x');

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    CHECK(result->native.pnp_path_utf8.availability == metadata_availability::capacity_exceeded);
    CHECK(result->native.pnp_path_utf8.value.empty());
    CHECK(result->transport.availability == metadata_availability::unavailable);
    CHECK(os.pnp_calls == 0U);

    auto haptic_request = make_request(container);
    haptic_request.facts.haptic_endpoint_id = {.availability = metadata_availability::available,
        .value = std::u16string(haptic_endpoint_content_code_units + 1U, u'h'),
        .native_code = 0U};
    const auto haptic_result = provider->query(haptic_request);
    REQUIRE(haptic_result.has_value());
    CHECK(haptic_result->native.haptic_endpoint_id.availability ==
          metadata_availability::capacity_exceeded);
    CHECK(
        haptic_result->headset_render.id.availability == metadata_availability::capacity_exceeded);
    CHECK(os.render_calls == 1U);
}

TEST_CASE("metadata.provider_sanitizes_unavailable_native_fields", "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    auto request = make_request(container);
    request.facts.vendor_id = {.availability = metadata_availability::unavailable,
        .value = 0x054CU,
        .native_code = 71U};
    request.facts.gameinput_device_id = {.availability = metadata_availability::unavailable,
        .value = {},
        .native_code = 72U};
    request.facts.gameinput_device_id.value.bytes[0U] = std::byte{0xA5U};
    request.facts.pnp_path_utf8.value.assign("invalid\0path", 12U);
    request.facts.haptic_endpoint_id.availability = metadata_availability::available;
    request.facts.haptic_endpoint_id.value = {u'h', u'\0', u'x'};

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE(result.has_value());
    CHECK(result->native.vendor_id.availability == metadata_availability::unavailable);
    CHECK(result->native.vendor_id.value == 0U);
    CHECK(result->native.gameinput_device_id.availability == metadata_availability::unavailable);
    CHECK(std::to_integer<unsigned int>(result->native.gameinput_device_id.value.bytes[0U]) == 0U);
    CHECK(result->native.pnp_path_utf8.availability == metadata_availability::unavailable);
    CHECK(result->native.pnp_path_utf8.value.empty());
    CHECK(result->native.haptic_endpoint_id.availability == metadata_availability::unavailable);
    CHECK(result->native.haptic_endpoint_id.value.empty());
    CHECK(os.pnp_calls == 0U);
}

TEST_CASE("metadata.provider_marks_oversized_available_endpoint_as_capacity_exceeded",
    "[component][metadata]") {
    const auto container = make_guid(1U);
    scripted_windows_os os{};
    os.pnp.container_id = {.availability = metadata_availability::available,
        .value = container,
        .native_code = 0U};
    os.render_endpoints.push_back(
        make_endpoint(container, std::u16string(headset_endpoint_content_code_units + 1U, u'x')));

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(make_request(container));

    REQUIRE(result.has_value());
    CHECK(result->headset_render.id.availability == metadata_availability::capacity_exceeded);
    CHECK(result->headset_render.id.value.empty());
}

TEST_CASE("metadata.provider_rejects_invalid_exact_targets_before_os_work",
    "[component][metadata]") {
    scripted_windows_os os{};
    auto request = make_request();
    request.target = {};

    const auto provider = make_windows_metadata_provider(os);
    const auto result = provider->query(request);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == metadata_error::not_connected);
    CHECK(os.pnp_calls == 0U);
    CHECK(os.render_calls == 0U);
    CHECK(os.capture_calls == 0U);
}

} // namespace
