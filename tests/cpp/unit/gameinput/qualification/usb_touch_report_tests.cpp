#include "../../../../support/values/required_value.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "input_qualification/usb_touch_report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

using namespace inputnext::input_qualification::diagnostic;
using inputnext::test::required_value;

using packet_type = std::array<std::uint8_t, dualsense_usb_input_report_size>;

struct contact_spec final {
    std::size_t slot_index{};
    std::uint8_t contact_id{};
    std::uint16_t x_coordinate{};
    std::uint16_t y_coordinate{};
    bool active{true};
};

[[nodiscard]] packet_type empty_packet(const bool click_pressed = false) noexcept {
    packet_type packet{};
    packet[0U] = static_cast<std::uint8_t>(dualsense_usb_input_report_id);
    packet[10U] = click_pressed ? 0x02U : 0U;
    packet[33U] = 0x80U;
    packet[37U] = 0x80U;
    return packet;
}

void set_contact(packet_type& packet, const contact_spec& contact) noexcept {
    const std::size_t offset = 33U + (contact.slot_index * 4U);
    const auto encoded_id = static_cast<std::uint32_t>(contact.contact_id) & 0x7FU;
    const auto inactive_flag = contact.active ? 0U : 0x80U;
    const auto x_coordinate = static_cast<std::uint32_t>(contact.x_coordinate);
    const auto y_coordinate = static_cast<std::uint32_t>(contact.y_coordinate);
    packet[offset] = static_cast<std::uint8_t>(encoded_id | inactive_flag);
    packet[offset + 1U] = static_cast<std::uint8_t>(x_coordinate & 0xFFU);
    packet[offset + 2U] =
        static_cast<std::uint8_t>(((x_coordinate >> 8U) & 0x0FU) | ((y_coordinate & 0x0FU) << 4U));
    packet[offset + 3U] = static_cast<std::uint8_t>(y_coordinate >> 4U);
}

[[nodiscard]] std::optional<inputnext::input::touch_observation> observation_from(
    const packet_type& packet,
    const bool click_pressed,
    const std::uint64_t timestamp) noexcept {
    const auto decoded = decode_dualsense_usb_touch(packet, click_pressed);
    if (!decoded.has_value()) {
        return std::nullopt;
    }

    inputnext::input::touch_observation observation{};
    observation.click_status = inputnext::input::observation_status::observed;
    observation.click_pressed = click_pressed;
    observation.contacts_status = inputnext::input::observation_status::observed;
    observation.same_reading = true;
    observation.contacts_complete = true;
    observation.surface_width = dualsense_usb_touch_width;
    observation.surface_height = dualsense_usb_touch_height;
    observation.contacts = decoded->contacts;
    observation.contact_count = decoded->count;
    observation.source_timestamp = timestamp;
    return observation;
}

} // namespace

TEST_CASE("gameinput.dualsense_usb_touch_decodes_exact_half_open_coordinate_bounds",
    "[unit][gameinput][qualification][touch]") {
    auto packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U, .contact_id = 127U, .x_coordinate = 0U, .y_coordinate = 0U});
    set_contact(packet,
        contact_spec{.slot_index = 1U,
            .contact_id = 3U,
            .x_coordinate = 1919U,
            .y_coordinate = 1079U});

    const auto decoded = decode_dualsense_usb_touch(packet, false);
    REQUIRE(decoded.has_value());
    const auto decoded_value = required_value(decoded);
    REQUIRE(decoded_value.count == 2U);
    CHECK(decoded_value.contacts[0U].id == 127U);
    CHECK(decoded_value.contacts[0U].x == 0.0);
    CHECK(decoded_value.contacts[0U].y == 0.0);
    CHECK(decoded_value.contacts[0U].active);
    CHECK(decoded_value.contacts[1U].id == 3U);
    CHECK(decoded_value.contacts[1U].x == 1919.0);
    CHECK(decoded_value.contacts[1U].y == 1079.0);
    CHECK(decoded_value.contacts[1U].active);

    packet = empty_packet(true);
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 4U,
            .x_coordinate = 960U,
            .y_coordinate = 540U});
    CHECK(decode_dualsense_usb_touch(packet, true).has_value());
}

TEST_CASE("gameinput.dualsense_usb_touch_ignores_inactive_stale_slots_and_allows_empty",
    "[unit][gameinput][qualification][touch]") {
    auto packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 7U,
            .x_coordinate = 4095U,
            .y_coordinate = 4095U,
            .active = false});
    set_contact(packet,
        contact_spec{.slot_index = 1U,
            .contact_id = 7U,
            .x_coordinate = 600U,
            .y_coordinate = 400U});

    const auto decoded = decode_dualsense_usb_touch(packet, false);
    REQUIRE(decoded.has_value());
    const auto decoded_value = required_value(decoded);
    REQUIRE(decoded_value.count == 1U);
    CHECK(decoded_value.contacts[0U].id == 7U);
    CHECK(decoded_value.contacts[0U].x == 600.0);
    CHECK(decoded_value.contacts[0U].y == 400.0);

    packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 127U,
            .x_coordinate = 4095U,
            .y_coordinate = 4095U,
            .active = false});
    set_contact(packet,
        contact_spec{.slot_index = 1U,
            .contact_id = 127U,
            .x_coordinate = 4095U,
            .y_coordinate = 4095U,
            .active = false});
    const auto empty = decode_dualsense_usb_touch(packet, false);
    REQUIRE(empty.has_value());
    CHECK(required_value(empty).count == 0U);
}

TEST_CASE("gameinput.dualsense_usb_touch_rejects_incomplete_mismatched_and_invalid_reports",
    "[unit][gameinput][qualification][touch]") {
    auto packet = empty_packet();
    CHECK_FALSE(
        decode_dualsense_usb_touch(std::span<const std::uint8_t>{packet.data(), packet.size() - 1U},
            false));
    std::array<std::uint8_t, dualsense_usb_input_report_size + 1U> oversized{};
    std::ranges::copy(packet, oversized.begin());
    CHECK_FALSE(decode_dualsense_usb_touch(oversized, false));

    packet[0U] = 2U;
    CHECK_FALSE(decode_dualsense_usb_touch(packet, false));

    packet = empty_packet(true);
    CHECK_FALSE(decode_dualsense_usb_touch(packet, false));

    packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 5U,
            .x_coordinate = 100U,
            .y_coordinate = 100U});
    set_contact(packet,
        contact_spec{.slot_index = 1U,
            .contact_id = 5U,
            .x_coordinate = 200U,
            .y_coordinate = 200U});
    CHECK_FALSE(decode_dualsense_usb_touch(packet, false));

    packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 8U,
            .x_coordinate = static_cast<std::uint16_t>(dualsense_usb_touch_width),
            .y_coordinate = 0U});
    CHECK_FALSE(decode_dualsense_usb_touch(packet, false));

    packet = empty_packet();
    set_contact(packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 9U,
            .x_coordinate = 0U,
            .y_coordinate = static_cast<std::uint16_t>(dualsense_usb_touch_height)});
    CHECK_FALSE(decode_dualsense_usb_touch(packet, false));
}

TEST_CASE("gameinput.dualsense_usb_touch_resolver_splits_latches_and_falls_back",
    "[unit][gameinput][qualification][touch]") {
    inputnext::input::touch_resolver resolver{};
    const auto released_packet = empty_packet();
    const auto released = observation_from(released_packet, false, 100U);
    REQUIRE(released.has_value());
    const auto released_observation = required_value(released);
    static_cast<void>(resolver.observe(released_observation));

    auto left_packet = empty_packet(true);
    set_contact(left_packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 1U,
            .x_coordinate = 100U,
            .y_coordinate = 400U});
    const auto left = observation_from(left_packet, true, 200U);
    REQUIRE(left.has_value());
    const auto left_observation = required_value(left);
    CHECK(resolver.observe(left_observation).source == inputnext::input::touch_source::left);

    auto moved_packet = empty_packet(true);
    set_contact(moved_packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 1U,
            .x_coordinate = 1800U,
            .y_coordinate = 400U});
    const auto moved = observation_from(moved_packet, true, 300U);
    REQUIRE(moved.has_value());
    const auto moved_observation = required_value(moved);
    CHECK(resolver.observe(moved_observation).source == inputnext::input::touch_source::left);

    const auto next_release_packet = empty_packet();
    const auto next_release = observation_from(next_release_packet, false, 400U);
    REQUIRE(next_release.has_value());
    const auto next_release_observation = required_value(next_release);
    static_cast<void>(resolver.observe(next_release_observation));

    auto right_packet = empty_packet(true);
    set_contact(right_packet,
        contact_spec{.slot_index = 0U,
            .contact_id = 2U,
            .x_coordinate = 1919U,
            .y_coordinate = 1079U});
    const auto right = observation_from(right_packet, true, 500U);
    REQUIRE(right.has_value());
    const auto right_observation = required_value(right);
    CHECK(resolver.observe(right_observation).source == inputnext::input::touch_source::right);

    inputnext::input::touch_resolver fallback_resolver{};
    REQUIRE(fallback_resolver.observe(released_observation).source ==
            inputnext::input::touch_source::none);
    const auto no_contact_packet = empty_packet(true);
    const auto no_contact = observation_from(no_contact_packet, true, 600U);
    REQUIRE(no_contact.has_value());
    const auto no_contact_observation = required_value(no_contact);
    CHECK(fallback_resolver.observe(no_contact_observation).source ==
          inputnext::input::touch_source::fallback);
}
