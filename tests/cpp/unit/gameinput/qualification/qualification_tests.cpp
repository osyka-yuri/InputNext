#include "gameinput/input_profile.hpp"
#include "gameinput/input_qualification.hpp"

#include <catch2/catch_test_macros.hpp>

#include <inputnext/device_metadata.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace inputnext;
using namespace inputnext::gameinput_detail;

namespace {

[[nodiscard]] input_qualification_facts candidate_facts() noexcept {
    static constexpr std::array<std::uint32_t, 15U>
        labels{80U, 78U, 79U, 81U, 112U, 118U, 0U, 0U, 100U, 99U, 114U, 120U, 0U, 0U, 0U};
    return input_qualification_facts{
        .runtime_version = {.availability = metadata_availability::available,
            .value = {.major = 4U, .minor = 1U, .build = 314U, .revision = 0U}},
        .hardware_version = {.availability = metadata_availability::available,
            .value = {.major = 0U, .minor = 0U, .build = 0U, .revision = 0U}},
        .firmware_version = {.availability = metadata_availability::available,
            .value = {.major = 0U, .minor = 0U, .build = 0U, .revision = 0U}},
        .vendor_id = {.availability = metadata_availability::available, .value = 0x054CU},
        .product_id = {.availability = metadata_availability::available, .value = 0x0CE6U},
        .revision = {.availability = metadata_availability::available, .value = 256U},
        .supported_input_bits = 22'282'319U,
        .gamepad_supported = true,
        .controller_button_supported = true,
        .button_count = 15U,
        .button_labels_complete = true,
        .button_labels = std::span<const std::uint32_t>{labels},
        .touch_surface_count = 1U,
        .touchpad_surface =
            touchpad_surface_shape{.width = 1920U, .height = 1080U, .max_contacts = 2U},
        .transport = {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::native_bus_type}},
    };
}

void require_gamepad_fallback(const input_qualification_facts& facts,
    const input_fallback_reason reason) {
    const auto profile = qualify_native_input(facts);
    CHECK(profile.history == input_history_path::gamepad);
    CHECK(profile.reason == reason);
    CHECK(!profile.touch_enabled());
    CHECK(profile.coordinates == touch_coordinate_source::none);

    std::array<std::uint16_t, 4U> expected_runtime_version{};
    if (facts.runtime_version.has_value()) {
        const auto& version = facts.runtime_version.value;
        expected_runtime_version = {version.major, version.minor, version.build, version.revision};
    }
    const auto expected_transport =
        facts.transport.has_value() ? facts.transport.value.value : physical_transport::unknown;
    CHECK(profile.runtime_version == expected_runtime_version);
    CHECK(profile.transport == expected_transport);
}

} // namespace

TEST_CASE("gameinput.qualification_accepts_GameInput_4_1_USB_touchpad",
    "[unit][gameinput][qualification]") {
    auto facts = candidate_facts();
    std::array<std::uint32_t, 15U> labels{};
    std::ranges::copy(facts.button_labels, labels.begin());
    facts.button_labels = labels;

    SECTION("labeled click on the observed x86 layout") {
        labels[13U] = gameinput_touchpad_button_label;
    }
    SECTION("unlabeled click on the observed x64 layout") {
    }
    SECTION("USB identified by the native enumerator") {
        facts.transport.value.provenance = transport_provenance::native_enumerator;
    }
    const auto profile = qualify_native_input(facts);
    CHECK(profile.history == input_history_path::composite);
    CHECK(profile.reason == input_fallback_reason::none);
    CHECK(profile.callback_coherent);
    CHECK(profile.touch_enabled());
    CHECK(profile.click_button == 13U);
    CHECK(profile.button_count == 15U);
    CHECK(profile.coordinates == touch_coordinate_source::surface);
    CHECK(profile.surface_index == 0U);
    CHECK(profile.surface_width == 1920U);
    CHECK(profile.surface_height == 1080U);
    CHECK(profile.runtime_version == std::array<std::uint16_t, 4U>{4U, 1U, 314U, 0U});
    CHECK(profile.transport == physical_transport::usb);
}

TEST_CASE("gameinput.qualification_rejects_unverified_GameInput_4_1_touchpad",
    "[unit][gameinput][qualification]") {
    auto facts = candidate_facts();
    std::array<std::uint32_t, 15U> labels{};
    std::ranges::copy(facts.button_labels, labels.begin());
    facts.button_labels = labels;

    SECTION("missing or unsupported surface descriptor") {
        facts.touchpad_surface.reset();
    }
    SECTION("zero surfaces") {
        facts.touch_surface_count = 0U;
    }
    SECTION("multiple surfaces") {
        facts.touch_surface_count = 2U;
    }
    SECTION("different surface width") {
        facts.touchpad_surface =
            touchpad_surface_shape{.width = 1919U, .height = 1080U, .max_contacts = 2U};
    }
    SECTION("different surface height") {
        facts.touchpad_surface =
            touchpad_surface_shape{.width = 1920U, .height = 1079U, .max_contacts = 2U};
    }
    SECTION("different contact capacity") {
        facts.touchpad_surface =
            touchpad_surface_shape{.width = 1920U, .height = 1080U, .max_contacts = 3U};
    }
    SECTION("the only touchpad label is at index 14") {
        labels[14U] = gameinput_touchpad_button_label;
    }
    SECTION("multiple touchpad button labels") {
        labels[13U] = gameinput_touchpad_button_label;
        labels[14U] = gameinput_touchpad_button_label;
    }
    SECTION("a touch label replaces a distinct control") {
        labels[9U] = gameinput_touchpad_button_label;
    }
    SECTION("the unlabeled layout changes") {
        labels[13U] = 1U;
    }
    SECTION("a different runtime reports the same descriptor") {
        facts.runtime_version.value.build = 315U;
    }
    SECTION("the descriptor from the former raw USB profile") {
        facts.runtime_version.value = {.major = 4U, .minor = 0U, .build = 306U, .revision = 0U};
        facts.revision.value = 100U;
        facts.supported_input_bits = 18'088'015U;
        facts.touch_surface_count = 0U;
        facts.touchpad_surface.reset();
    }
    SECTION("a different controller reports the same descriptor") {
        facts.product_id.value = 0x0DF2U;
    }
    SECTION("the descriptor appears on Bluetooth") {
        facts.transport.value.value = physical_transport::bluetooth;
    }

    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);
}

TEST_CASE("gameinput.qualification_rejects_missing_versions_and_transport_evidence",
    "[unit][gameinput][qualification]") {
    auto facts = candidate_facts();
    facts.runtime_version.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.hardware_version.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.firmware_version.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.vendor_id.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.product_id.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.revision.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.transport.value.provenance = transport_provenance::pnp_path_heuristic;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.transport.availability = metadata_availability::unavailable;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.transport.value.value = physical_transport::bluetooth;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.transport.value.value = physical_transport::other_wireless;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.transport.value.value = physical_transport::other;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);
}

TEST_CASE("gameinput.qualification_rejects_device_version_identifier_and_capability_changes",
    "[unit][gameinput][qualification]") {
    auto facts = candidate_facts();
    facts.runtime_version.value.revision = 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.runtime_version.value.build = 313U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.hardware_version.value.revision = 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.firmware_version.value.revision = 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.vendor_id.value ^= 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.product_id.value ^= 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.revision.value = 257U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.supported_input_bits ^= 1U;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.gamepad_supported = false;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);

    facts = candidate_facts();
    facts.controller_button_supported = false;
    require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);
}

TEST_CASE("gameinput.qualification_rejects_incomplete_or_changed_descriptor",
    "[unit][gameinput][qualification]") {
    auto facts = candidate_facts();
    facts.button_labels_complete = false;
    require_gamepad_fallback(facts, input_fallback_reason::incoherent_reading);

    facts = candidate_facts();
    facts.button_count = 0U;
    facts.button_labels = {};
    require_gamepad_fallback(facts, input_fallback_reason::incoherent_reading);

    facts = candidate_facts();
    facts.button_count = 14U;
    require_gamepad_fallback(facts, input_fallback_reason::incoherent_reading);

    facts = candidate_facts();
    facts.button_labels = facts.button_labels.first(14U);
    require_gamepad_fallback(facts, input_fallback_reason::incoherent_reading);

    const auto expected_button_count = facts.button_count;
    for (std::size_t changed_index = 0U; changed_index < expected_button_count; ++changed_index) {
        facts = candidate_facts();
        std::array<std::uint32_t, 15U> changed_labels{};
        std::ranges::copy(facts.button_labels, changed_labels.begin());
        changed_labels[changed_index] ^= 1U;
        facts.button_labels = std::span<const std::uint32_t>{changed_labels};
        require_gamepad_fallback(facts, input_fallback_reason::unsupported_capability);
    }

    facts = candidate_facts();
    facts.button_count = 65U;
    facts.button_labels = {};
    require_gamepad_fallback(facts, input_fallback_reason::incoherent_reading);
}
