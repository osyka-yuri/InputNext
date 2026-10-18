#include "policy.hpp"

#include <inputnext/device_metadata.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace inputnext::metadata_detail {
namespace {

constexpr std::uint16_t sony_vendor_id = 0x054CU;
constexpr std::uint16_t dualsense_product_id = 0x0CE6U;
constexpr std::uint16_t dualsense_edge_product_id = 0x0DF2U;

[[nodiscard]] constexpr unsigned char ascii_lower(const unsigned char value) noexcept {
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(
            value + (static_cast<unsigned char>('a') - static_cast<unsigned char>('A')));
    }
    return value;
}

[[nodiscard]] bool contains_ascii_insensitive(const std::string_view value,
    const std::string_view token) noexcept {
    if (token.empty()) {
        return true;
    }
    if (token.size() > value.size()) {
        return false;
    }
    for (std::size_t start = 0U; start <= value.size() - token.size(); ++start) {
        bool matches = true;
        for (std::size_t index = 0U; index < token.size(); ++index) {
            const auto left = ascii_lower(static_cast<unsigned char>(value[start + index]));
            const auto right = ascii_lower(static_cast<unsigned char>(token[index]));
            if (left != right) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool is_continuation(const unsigned char value) noexcept {
    return (value & 0xC0U) == 0x80U;
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t width = 0U;
        std::uint32_t code_point = 0U;
        std::uint32_t minimum = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            width = 2U;
            code_point = first & 0x1FU;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            width = 3U;
            code_point = first & 0x0FU;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            width = 4U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (width > value.size() - index) {
            return false;
        }
        for (std::size_t tail = 1U; tail < width; ++tail) {
            const auto continuation = static_cast<unsigned char>(value[index + tail]);
            if (!is_continuation(continuation)) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += width;
    }
    return true;
}

[[nodiscard]] std::uint32_t first_native_code(const native_device_facts& facts) noexcept {
    if (facts.vendor_id.native_code != 0U) {
        return facts.vendor_id.native_code;
    }
    if (facts.product_id.native_code != 0U) {
        return facts.product_id.native_code;
    }
    return facts.gameinput_capabilities.native_code;
}

[[nodiscard]] hardware_capabilities dualsense_capabilities(const bool edge) noexcept {
    return hardware_capabilities{
        .gamepad = true,
        .low_frequency_rumble = true,
        .high_frequency_rumble = true,
        .trigger_rumble = false,
        .adaptive_left_trigger = true,
        .adaptive_right_trigger = true,
        .lightbar = true,
        .player_leds = true,
        .microphone_led = true,
        .gyroscope = true,
        .accelerometer = true,
        .paddles = edge,
    };
}

} // namespace

metadata_field<std::string> copy_bounded_pnp_path(const std::string_view input,
    const std::uint32_t native_code) {
    if (input.empty()) {
        return {.availability = metadata_availability::not_present,
            .value = {},
            .native_code = native_code};
    }
    if (input.size() > max_pnp_path_utf8_bytes) {
        return {.availability = metadata_availability::capacity_exceeded,
            .value = {},
            .native_code = native_code};
    }
    if (input.contains('\0') || !valid_utf8(input)) {
        return {.availability = metadata_availability::unavailable,
            .value = {},
            .native_code = native_code};
    }
    return {.availability = metadata_availability::available,
        .value = std::string{input},
        .native_code = native_code};
}

metadata_field<std::u16string> copy_bounded_utf16(const utf16_buffer_view input,
    const std::size_t max_content_code_units) {
    if (input.data == nullptr || input.code_units == 0U) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    const auto* const terminator = std::find(input.data, input.data + input.code_units, u'\0');
    if (terminator == input.data + input.code_units) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    const auto content_length = static_cast<std::size_t>(terminator - input.data);
    if (content_length == 0U) {
        return {.availability = metadata_availability::not_present, .value = {}, .native_code = 0U};
    }
    if (content_length > max_content_code_units) {
        return {.availability = metadata_availability::capacity_exceeded,
            .value = {},
            .native_code = 0U};
    }
    return {.availability = metadata_availability::available,
        .value = std::u16string{input.data, content_length},
        .native_code = 0U};
}

metadata_field<std::u16string> copy_bounded_utf16_content(const std::u16string_view input,
    const std::size_t max_content_code_units) {
    if (input.empty()) {
        return {.availability = metadata_availability::not_present, .value = {}, .native_code = 0U};
    }
    if (input.size() > max_content_code_units) {
        return {.availability = metadata_availability::capacity_exceeded,
            .value = {},
            .native_code = 0U};
    }
    if (input.contains(u'\0')) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    return {.availability = metadata_availability::available,
        .value = std::u16string{input},
        .native_code = 0U};
}

metadata_field<transport_metadata> infer_transport_from_pnp_path(const std::string_view pnp_path) {
    if (pnp_path.empty() || pnp_path.size() > max_pnp_path_utf8_bytes || pnp_path.contains('\0') ||
        !valid_utf8(pnp_path)) {
        return {.availability = metadata_availability::unavailable, .value = {}, .native_code = 0U};
    }
    if (contains_ascii_insensitive(pnp_path, "bth") ||
        contains_ascii_insensitive(pnp_path, "bluetooth")) {
        return {.availability = metadata_availability::available,
            .value = {.value = physical_transport::bluetooth,
                .provenance = transport_provenance::pnp_path_heuristic},
            .native_code = 0U};
    }
    if (contains_ascii_insensitive(pnp_path, "wireless") ||
        contains_ascii_insensitive(pnp_path, "wlan")) {
        return {.availability = metadata_availability::available,
            .value = {.value = physical_transport::other_wireless,
                .provenance = transport_provenance::pnp_path_heuristic},
            .native_code = 0U};
    }
    if (contains_ascii_insensitive(pnp_path, "usb") ||
        contains_ascii_insensitive(pnp_path, "xusb")) {
        return {.availability = metadata_availability::available,
            .value = {.value = physical_transport::usb,
                .provenance = transport_provenance::pnp_path_heuristic},
            .native_code = 0U};
    }
    return {.availability = metadata_availability::unavailable,
        .value = {.value = physical_transport::unknown,
            .provenance = transport_provenance::unavailable},
        .native_code = 0U};
}

metadata_field<metadata_device_model> classify_device_model(
    const native_device_facts& facts) noexcept {
    if (facts.vendor_id.has_value() && facts.product_id.has_value() &&
        facts.vendor_id.value == sony_vendor_id) {
        if (facts.product_id.value == dualsense_product_id) {
            return {.availability = metadata_availability::available,
                .value = metadata_device_model::dualsense,
                .native_code = 0U};
        }
        if (facts.product_id.value == dualsense_edge_product_id) {
            return {.availability = metadata_availability::available,
                .value = metadata_device_model::dualsense_edge,
                .native_code = 0U};
        }
    }
    if (facts.gameinput_capabilities.has_value() && facts.gameinput_capabilities.value.gamepad) {
        return {.availability = metadata_availability::available,
            .value = metadata_device_model::generic_gamepad,
            .native_code = 0U};
    }
    if (facts.gameinput_capabilities.has_value()) {
        return {.availability = metadata_availability::not_present,
            .value = metadata_device_model::generic_gamepad,
            .native_code = 0U};
    }
    return {.availability = metadata_availability::unavailable,
        .value = metadata_device_model::generic_gamepad,
        .native_code = first_native_code(facts)};
}

metadata_field<hardware_capabilities> derive_hardware_capabilities(
    const native_device_facts& facts) noexcept {
    const auto model = classify_device_model(facts);
    if (model.has_value()) {
        if (model.value == metadata_device_model::dualsense) {
            return {.availability = metadata_availability::available,
                .value = dualsense_capabilities(false),
                .native_code = 0U};
        }
        if (model.value == metadata_device_model::dualsense_edge) {
            return {.availability = metadata_availability::available,
                .value = dualsense_capabilities(true),
                .native_code = 0U};
        }
    }
    if (model.availability == metadata_availability::not_present) {
        return {.availability = metadata_availability::not_present,
            .value = {},
            .native_code = model.native_code};
    }
    if (!facts.gameinput_capabilities.has_value()) {
        return {.availability = facts.gameinput_capabilities.availability,
            .value = {},
            .native_code = facts.gameinput_capabilities.native_code};
    }
    const auto& source = facts.gameinput_capabilities.value;
    return {.availability = metadata_availability::available,
        .value =
            hardware_capabilities{
                .gamepad = source.gamepad,
                .low_frequency_rumble = source.low_frequency_rumble,
                .high_frequency_rumble = source.high_frequency_rumble,
                .trigger_rumble = source.left_trigger_rumble || source.right_trigger_rumble,
            },
        .native_code = facts.gameinput_capabilities.native_code};
}

} // namespace inputnext::metadata_detail
