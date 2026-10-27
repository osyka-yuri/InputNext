#include "effects_projection.hpp"

#include "core/core.hpp"
#include "core_api_projection.hpp"
#include "effects/effects.hpp"
#include "effects_projection_support.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace inputnext::runtime_detail::effects_projection {
namespace {

using effects_projection_support::write_channels;
static_assert(sizeof(input_next_effects_trigger_v1) == 20U);
static_assert(sizeof(input_next_effects_rumble_v1) == 8U);
static_assert(sizeof(input_next_effects_led_v1) == 16U);
static_assert(sizeof(input_next_effects_capabilities_v1) == 32U);
static_assert(sizeof(input_next_effects_open_v1) == 48U);
static_assert(sizeof(input_next_effects_context_info_v1) == 40U);
static_assert(sizeof(input_next_effects_request_v1) == 120U);
static_assert(sizeof(input_next_effects_receipt_v1) == 88U);
static_assert(sizeof(input_next_effects_clear_v1) == 80U);
static_assert(sizeof(input_next_effects_pcm_v1) == 32U);
static_assert(sizeof(input_next_effects_body_request_v1) == 112U);
static_assert(sizeof(input_next_effects_body_receipt_v1) == 120U);
static_assert(sizeof(input_next_effects_body_clear_v1) == 64U);
static_assert(sizeof(input_next_effects_close_v1) == 48U);
static_assert(sizeof(input_next_effects_close_receipt_v1) == 96U);
static_assert(sizeof(input_next_effects_progress_query_v1) == 56U);
static_assert(sizeof(input_next_effects_progress_v1) == 80U);
static_assert(sizeof(input_next_effects_api_v1) == (8U + (10U * sizeof(void*))));
static_assert(offsetof(input_next_effects_pcm_v1, samples) == 24U);
#if UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(input_next_effects_pcm_v1, pointer_padding) == 28U);
#endif

[[nodiscard]] bool write_completion_capabilities(const effects::completion_capabilities value,
    std::uint32_t& out) noexcept {
    const auto raw = static_cast<std::uint32_t>(value);
    constexpr auto known =
        static_cast<std::uint32_t>(effects::completion_capabilities::setter_returned_current) |
        static_cast<std::uint32_t>(effects::completion_capabilities::raw_output_accepted_current);
    if ((raw & ~known) != 0U) {
        return false;
    }
    out = 0U;
    if ((raw & static_cast<std::uint32_t>(
                   effects::completion_capabilities::setter_returned_current)) != 0U) {
        out |= INPUT_NEXT_EFFECTS_COMPLETION_CAPABILITY_SETTER_RETURNED_CURRENT;
    }
    if ((raw & static_cast<std::uint32_t>(
                   effects::completion_capabilities::raw_output_accepted_current)) != 0U) {
        out |= INPUT_NEXT_EFFECTS_COMPLETION_CAPABILITY_RAW_OUTPUT_ACCEPTED_CURRENT;
    }
    return true;
}

[[nodiscard]] bool write_features(const effects::feature_mask value, std::uint32_t& out) noexcept {
    const auto raw = value;
    constexpr auto known = 0x1FFU;
    if ((raw & ~known) != 0U) {
        return false;
    }

    out = 0U;
    if (effects::has_feature(value, effects::feature_flags::left_trigger)) {
        out |= INPUT_NEXT_EFFECTS_CAP_LEFT_TRIGGER;
    }
    if (effects::has_feature(value, effects::feature_flags::right_trigger)) {
        out |= INPUT_NEXT_EFFECTS_CAP_RIGHT_TRIGGER;
    }
    if (effects::has_feature(value, effects::feature_flags::low_rumble)) {
        out |= INPUT_NEXT_EFFECTS_CAP_LOW_RUMBLE;
    }
    if (effects::has_feature(value, effects::feature_flags::high_rumble)) {
        out |= INPUT_NEXT_EFFECTS_CAP_HIGH_RUMBLE;
    }
    if (effects::has_feature(value, effects::feature_flags::lightbar)) {
        out |= INPUT_NEXT_EFFECTS_CAP_LIGHTBAR;
    }
    if (effects::has_feature(value, effects::feature_flags::player_leds)) {
        out |= INPUT_NEXT_EFFECTS_CAP_PLAYER_LEDS;
    }
    if (effects::has_feature(value, effects::feature_flags::microphone_led)) {
        out |= INPUT_NEXT_EFFECTS_CAP_MIC_LED;
    }
    if (effects::has_feature(value, effects::feature_flags::body_lease)) {
        out |= INPUT_NEXT_EFFECTS_CAP_BODY_LEASE;
    }
    if (effects::has_feature(value, effects::feature_flags::hd_ready)) {
        out |= INPUT_NEXT_EFFECTS_CAP_HD_READY;
    }
    return true;
}
} // namespace

input_next_status to_c_status(const effects::status value) noexcept {
    switch (value) {
    case effects::status::ok:
        return INPUT_NEXT_STATUS_OK;
    case effects::status::invalid_argument:
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    case effects::status::unavailable:
        return INPUT_NEXT_STATUS_UNAVAILABLE;
    case effects::status::stale_target:
        return INPUT_NEXT_STATUS_STALE_TARGET;
    case effects::status::unsupported_target:
        return INPUT_NEXT_STATUS_UNSUPPORTED_TARGET;
    case effects::status::unsupported_channel:
        return INPUT_NEXT_STATUS_UNSUPPORTED_CHANNEL;
    case effects::status::scheduler_unavailable:
        return INPUT_NEXT_STATUS_SCHEDULER_UNAVAILABLE;
    case effects::status::busy:
        return INPUT_NEXT_STATUS_BUSY;
    case effects::status::stale_lease:
        return INPUT_NEXT_STATUS_STALE_LEASE;
    case effects::status::capacity_exceeded:
        return INPUT_NEXT_STATUS_CAPACITY_EXCEEDED;
    case effects::status::stale_context:
        return INPUT_NEXT_STATUS_STALE_CONTEXT;
    case effects::status::stale_effect:
        return INPUT_NEXT_STATUS_STALE_EFFECT;
    case effects::status::stale_sequence:
        return INPUT_NEXT_STATUS_STALE_SEQUENCE;
    case effects::status::source_retired:
        return INPUT_NEXT_STATUS_SOURCE_RETIRED;
    case effects::status::unsupported:
        return INPUT_NEXT_STATUS_UNSUPPORTED;
    case effects::status::io_failed:
        return INPUT_NEXT_STATUS_IO_FAILED;
    case effects::status::revoked:
        return INPUT_NEXT_STATUS_REVOKED;
    case effects::status::pending:
        return INPUT_NEXT_STATUS_PENDING;
    case effects::status::no_data:
        return INPUT_NEXT_STATUS_NO_DATA;
    case effects::status::not_admitted:
        return INPUT_NEXT_STATUS_NOT_ADMITTED;
    }
    return INPUT_NEXT_STATUS_IO_FAILED;
}

bool parse_context(const input_next_effects_context_v1& value,
    effects::context_handle& out) noexcept {
    out = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U || value.id == 0U) {
        return false;
    }
    std::memcpy(out.runtime.bytes.data(), value.runtime_id, out.runtime.bytes.size());
    out.id = value.id;
    return out.runtime.valid();
}

void write_context(const effects::context_handle& value,
    input_next_effects_context_v1& out) noexcept {
    out = {};
    out.struct_size = sizeof(out);
    std::memcpy(out.runtime_id, value.runtime.bytes.data(), value.runtime.bytes.size());
    out.id = value.id;
}

bool valid_output_header(const input_next_device_target_v1& value) noexcept {
    return value.struct_size == sizeof(value) && value.reserved == 0U;
}

bool valid_output_header(const input_next_effects_capabilities_v1& value) noexcept {
    return value.struct_size == sizeof(value);
}

bool valid_output_header(const input_next_effects_context_v1& value) noexcept {
    return value.struct_size == sizeof(value) && value.reserved == 0U;
}

bool valid_output_header(const input_next_effects_context_info_v1& value) noexcept {
    return value.struct_size == sizeof(value) && valid_output_header(value.context);
}

bool valid_output_header(const input_next_effects_receipt_v1& value) noexcept {
    return value.struct_size == sizeof(value) && valid_output_header(value.context);
}

bool valid_output_header(const input_next_effects_body_receipt_v1& value) noexcept {
    return value.struct_size == sizeof(value) && value.reserved == 0U &&
           valid_output_header(value.context) && valid_output_header(value.target);
}

bool valid_output_header(const input_next_effects_close_receipt_v1& value) noexcept {
    return value.struct_size == sizeof(value) && valid_output_header(value.context) &&
           valid_output_header(value.target);
}

bool valid_output_header(const input_next_effects_progress_v1& value) noexcept {
    return value.struct_size == sizeof(value) && valid_output_header(value.target);
}

bool parse_open_request(const input_next_effects_open_v1& value, connection_target& out) noexcept {
    out = {};
    return value.struct_size == sizeof(value) && value.flags == 0U &&
           core_projection::parse_target(value.target, out);
}

bool parse_selector(const input_next_device_selector_v1& value, core::selector& out) noexcept {
    return core_projection::parse_selector(value, out);
}

bool write_target(const connection_target& value, input_next_device_target_v1& out) noexcept {
    if (!value.valid()) {
        return false;
    }
    core_projection::write_target(value, out);
    return true;
}

bool write_capabilities(const effects::capabilities& value,
    input_next_effects_capabilities_v1& out) noexcept {
    std::uint32_t channels{};
    std::uint32_t features{};
    std::uint32_t completion{};
    if ((value.supported_channels & ~output::supported_channel_mask) != 0U ||
        value.observation_revision == 0U || !write_features(value.features, features) ||
        !write_completion_capabilities(value.completion, completion)) {
        return false;
    }
    channels = write_channels(value.supported_channels);
    out = {};
    out.struct_size = sizeof(out);
    out.supported_channels = channels;
    out.feature_flags = features;
    out.completion_capabilities = completion;
    out.audio_instance_id = value.audio_instance_id;
    out.capabilities_revision = value.observation_revision;
    return true;
}

bool write_context_info(const effects::context_info& value,
    input_next_effects_context_info_v1& out) noexcept {
    if (!value.context.runtime.valid() || value.context.id == 0U) {
        return false;
    }
    std::uint32_t state{};
    switch (value.state) {
    case effects::context_state::open:
        state = INPUT_NEXT_EFFECTS_CONTEXT_OPEN;
        break;
    case effects::context_state::revoked:
        state = INPUT_NEXT_EFFECTS_CONTEXT_REVOKED;
        break;
    default:
        return false;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.state = state;
    write_context(value.context, out.context);
    return true;
}

} // namespace inputnext::runtime_detail::effects_projection
