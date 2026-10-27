#include "core_api_projection.hpp"

#include "core/core.hpp"
#include <input_next_core.h>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace inputnext::runtime_detail::core_projection {
namespace {

static_assert(static_cast<std::uint32_t>(inputnext::core::status::ok) == INPUT_NEXT_STATUS_OK);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::invalid_argument) ==
              INPUT_NEXT_STATUS_INVALID_ARGUMENT);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::unavailable) ==
              INPUT_NEXT_STATUS_UNAVAILABLE);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::stale_target) ==
              INPUT_NEXT_STATUS_STALE_TARGET);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::busy) == INPUT_NEXT_STATUS_BUSY);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::buffer_too_small) ==
              INPUT_NEXT_STATUS_BUFFER_TOO_SMALL);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::capacity_exceeded) ==
              INPUT_NEXT_STATUS_CAPACITY_EXCEEDED);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::stale_reader) ==
              INPUT_NEXT_STATUS_STALE_READER);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::source_retired) ==
              INPUT_NEXT_STATUS_SOURCE_RETIRED);
static_assert(
    static_cast<std::uint32_t>(inputnext::core::status::io_failed) == INPUT_NEXT_STATUS_IO_FAILED);
static_assert(
    static_cast<std::uint32_t>(inputnext::core::status::no_data) == INPUT_NEXT_STATUS_NO_DATA);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::ambiguous_target) ==
              INPUT_NEXT_STATUS_AMBIGUOUS_TARGET);
static_assert(static_cast<std::uint32_t>(inputnext::core::status::not_admitted) ==
              INPUT_NEXT_STATUS_NOT_ADMITTED);
static_assert(static_cast<std::uint32_t>(inputnext::core::identity_quality::none) ==
              INPUT_NEXT_CORE_IDENTITY_NONE);
static_assert(static_cast<std::uint32_t>(inputnext::core::identity_quality::platform) ==
              INPUT_NEXT_CORE_IDENTITY_PLATFORM);
static_assert(static_cast<std::uint32_t>(inputnext::core::identity_quality::ambiguous) ==
              INPUT_NEXT_CORE_IDENTITY_AMBIGUOUS);
static_assert(
    static_cast<std::uint32_t>(inputnext::history_loss::none) == INPUT_NEXT_CORE_READER_LOSS_NONE);
static_assert(static_cast<std::uint32_t>(inputnext::history_loss::known) ==
              INPUT_NEXT_CORE_READER_LOSS_KNOWN);
static_assert(static_cast<std::uint32_t>(inputnext::history_loss::unknown) ==
              INPUT_NEXT_CORE_READER_LOSS_UNKNOWN);
static_assert(static_cast<std::uint32_t>(inputnext::history_reset::none) ==
              INPUT_NEXT_CORE_READER_RESET_NONE);
static_assert(static_cast<std::uint32_t>(inputnext::history_reset::initial_open) ==
              INPUT_NEXT_CORE_READER_RESET_INITIAL_OPEN);
static_assert(static_cast<std::uint32_t>(inputnext::history_reset::history_gap) ==
              INPUT_NEXT_CORE_READER_RESET_HISTORY_GAP);

static_assert(sizeof(input_next_core_api_v1) == (8U + (7U * sizeof(void*))));
static_assert(sizeof(input_next_device_target_v1) == 40U);
static_assert(sizeof(input_next_input_domain_v1) == 48U);
static_assert(sizeof(input_next_handle_v1) == 32U);
static_assert(sizeof(input_next_reader_v1) == 32U);
static_assert(sizeof(input_next_effects_context_v1) == 32U);
static_assert(sizeof(input_next_runtime_info_v1) == 48U);
static_assert(sizeof(input_next_device_selector_v1) == 96U);
static_assert(sizeof(input_next_device_info_v1) == 232U);
static_assert(sizeof(input_next_gamepad_state_v1) == 28U);
static_assert(sizeof(input_next_input_snapshot_v1) == 112U);
static_assert(sizeof(input_next_reader_request_v1) == 56U);
static_assert(sizeof(input_next_reader_result_v1) == 112U);
static_assert(offsetof(input_next_core_api_v1, get_runtime_info) == 8U);
static_assert(offsetof(input_next_core_api_v1, enumerate_devices) == (8U + sizeof(void*)));
static_assert(offsetof(input_next_core_api_v1, resolve_target) == (8U + (2U * sizeof(void*))));
static_assert(offsetof(input_next_core_api_v1, read_latest) == (8U + (3U * sizeof(void*))));
static_assert(offsetof(input_next_core_api_v1, open_reader) == (8U + (4U * sizeof(void*))));
static_assert(offsetof(input_next_core_api_v1, read_events) == (8U + (5U * sizeof(void*))));
static_assert(offsetof(input_next_core_api_v1, close_reader) == (8U + (6U * sizeof(void*))));
static_assert(sizeof(input_next_core_api_v1) == (sizeof(void*) == 8U ? 64U : 36U));

void write_gamepad(const inputnext::gamepad_state& value,
    input_next_gamepad_state_v1& out) noexcept {
    out.buttons = value.buttons;
    out.left_x = value.left_stick.x;
    out.left_y = value.left_stick.y;
    out.right_x = value.right_stick.x;
    out.right_y = value.right_stick.y;
    out.left_trigger = value.left_trigger;
    out.right_trigger = value.right_trigger;
}

[[nodiscard]] bool valid_snapshot(const inputnext::history_record& value,
    const inputnext::history_domain& domain) noexcept {
    return domain.target.valid() && domain.stream != 0U && value.sequence != 0U &&
           inputnext::is_valid_gamepad_state(value.sample.state);
}

} // namespace

input_next_status to_c_status(const inputnext::core::status value) noexcept {
    switch (value) {
    case inputnext::core::status::ok:
    case inputnext::core::status::invalid_argument:
    case inputnext::core::status::unavailable:
    case inputnext::core::status::stale_target:
    case inputnext::core::status::busy:
    case inputnext::core::status::buffer_too_small:
    case inputnext::core::status::capacity_exceeded:
    case inputnext::core::status::stale_reader:
    case inputnext::core::status::source_retired:
    case inputnext::core::status::io_failed:
    case inputnext::core::status::no_data:
    case inputnext::core::status::ambiguous_target:
    case inputnext::core::status::not_admitted:
        return static_cast<input_next_status>(value);
    }
    return INPUT_NEXT_STATUS_UNAVAILABLE;
}

bool parse_selector(const input_next_device_selector_v1& value,
    inputnext::core::selector& out) noexcept {
    out = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U || value.reserved2 != 0U ||
        value.identity_flags != 0U || value.target.struct_size != sizeof(value.target) ||
        value.target.reserved != 0U) {
        return false;
    }

    switch (value.kind) {
    case INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT:
        if (value.xinput_slot >= 4U || !target_is_zero(value.target) ||
            !is_zero_bytes(value.hardware_fingerprint, sizeof(value.hardware_fingerprint))) {
            return false;
        }
        out.kind = inputnext::core::selector_kind::xinput_slot;
        out.slot = value.xinput_slot;
        return true;

    case INPUT_NEXT_CORE_SELECTOR_EXACT_TARGET:
        if (value.xinput_slot != 0U ||
            !is_zero_bytes(value.hardware_fingerprint, sizeof(value.hardware_fingerprint)) ||
            !parse_target(value.target, out.target)) {
            return false;
        }
        out.kind = inputnext::core::selector_kind::exact_target;
        return true;

    case INPUT_NEXT_CORE_SELECTOR_HARDWARE_FINGERPRINT:
        if (value.xinput_slot != 0U || !target_is_zero(value.target) ||
            is_zero_bytes(value.hardware_fingerprint, sizeof(value.hardware_fingerprint))) {
            return false;
        }
        out.kind = inputnext::core::selector_kind::hardware_fingerprint;
        std::copy_n(value.hardware_fingerprint,
            out.hardware_fingerprint.size(),
            out.hardware_fingerprint.begin());
        return true;

    default:
        return false;
    }
}

bool parse_target(const input_next_device_target_v1& value,
    inputnext::connection_target& out) noexcept {
    out = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U) {
        return false;
    }

    std::memcpy(out.runtime.bytes.data(), value.runtime_id, out.runtime.bytes.size());
    out.device.value = value.device_key;
    out.connection.value = value.connection_id;
    return out.valid();
}

bool target_is_zero(const input_next_device_target_v1& value) noexcept {
    return value.runtime_id[0] == 0U && value.runtime_id[1] == 0U && value.device_key == 0U &&
           value.connection_id == 0U;
}

bool is_zero_bytes(const std::uint8_t* const value, const std::size_t size) noexcept {
    return std::all_of(value, value + size, [](const std::uint8_t byte) { return byte == 0U; });
}

void write_target(const inputnext::connection_target& value,
    input_next_device_target_v1& out) noexcept {
    out = {};
    out.struct_size = sizeof(out);
    std::memcpy(out.runtime_id, value.runtime.bytes.data(), value.runtime.bytes.size());
    out.device_key = value.device.value;
    out.connection_id = value.connection.value;
}

void write_domain(const inputnext::history_domain& value,
    input_next_input_domain_v1& out) noexcept {
    out = {};
    out.struct_size = sizeof(out);
    std::memcpy(out.runtime_id,
        value.target.runtime.bytes.data(),
        value.target.runtime.bytes.size());
    out.device_key = value.target.device.value;
    out.connection_id = value.target.connection.value;
    out.stream_id = value.stream;
}

bool valid_device(const inputnext::core::device_record& value) noexcept {
    constexpr auto known_channels = inputnext::core::left_trigger_channel |
                                    inputnext::core::right_trigger_channel |
                                    inputnext::core::rumble_channel | inputnext::core::led_channel;
    const bool valid_transport = value.transport == inputnext::physical_transport::usb ||
                                 value.transport == inputnext::physical_transport::bluetooth ||
                                 value.transport == inputnext::physical_transport::other_wireless ||
                                 value.transport == inputnext::physical_transport::other ||
                                 value.transport == inputnext::physical_transport::unknown;
    const bool valid_identity = value.identity == inputnext::core::identity_quality::none ||
                                value.identity == inputnext::core::identity_quality::platform ||
                                value.identity == inputnext::core::identity_quality::ambiguous;
    const bool fingerprint_is_zero =
        is_zero_bytes(value.hardware_fingerprint.data(), value.hardware_fingerprint.size());
    return value.target.valid() && (value.output_channels & ~known_channels) == 0U &&
           valid_transport && valid_identity &&
           ((value.identity == inputnext::core::identity_quality::none) == fingerprint_is_zero);
}

void write_device(const inputnext::core::device_record& value,
    input_next_device_info_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    out.input_profile = INPUT_NEXT_CORE_INPUT_PROFILE_GAMEPAD;
    out.output_channels = value.output_channels;
    write_target(value.target, out.target);
    out.vendor_id = value.vendor_id;
    out.product_id = value.product_id;
    out.revision = value.revision;
    switch (value.transport) {
    case inputnext::physical_transport::usb:
        out.transport = INPUT_NEXT_CORE_TRANSPORT_USB;
        break;
    case inputnext::physical_transport::bluetooth:
        out.transport = INPUT_NEXT_CORE_TRANSPORT_BLUETOOTH;
        break;
    case inputnext::physical_transport::other_wireless:
        out.transport = INPUT_NEXT_CORE_TRANSPORT_OTHER_WIRELESS;
        break;
    case inputnext::physical_transport::other:
    case inputnext::physical_transport::unknown:
        out.transport = INPUT_NEXT_CORE_TRANSPORT_UNKNOWN;
        break;
    }
    out.identity_quality = static_cast<std::uint32_t>(value.identity);
    std::ranges::copy(value.hardware_fingerprint, out.hardware_fingerprint);
}

void write_snapshot(const inputnext::history_record& value,
    const inputnext::history_domain domain,
    input_next_input_snapshot_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    out.flags = value.baseline ? INPUT_NEXT_CORE_SNAPSHOT_BASELINE : 0U;
    write_domain(domain, out.domain);
    out.sequence = value.sequence;
    out.source_timestamp_us = value.sample.source_timestamp_us;
    out.qpc = value.sample.effective_qpc;
    write_gamepad(value.sample.state, out.state);
}

input_next_status write_reader_batch(const inputnext::core::reader_result& value,
    input_next_input_snapshot_v1* const out_snapshots,
    const std::size_t capacity,
    input_next_reader_result_v1& out_result) noexcept {
    const auto& batch = value.batch;
    if (batch.count > batch.records.size()) {
        return INPUT_NEXT_STATUS_UNAVAILABLE;
    }
    const bool buffer_too_small = value.outcome == inputnext::core::status::buffer_too_small;
    if (buffer_too_small != (batch.count > capacity)) {
        return INPUT_NEXT_STATUS_UNAVAILABLE;
    }
    if (!buffer_too_small) {
        for (std::size_t index = 0U; index < batch.count; ++index) {
            if (!valid_snapshot(batch.records[index], batch.domain)) {
                return INPUT_NEXT_STATUS_UNAVAILABLE;
            }
        }
    }

    write_reader_result(batch, out_result);
    if (buffer_too_small) {
        return INPUT_NEXT_STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0U; index < batch.count; ++index) {
        write_snapshot(batch.records[index], batch.domain, out_snapshots[index]);
    }
    return to_c_status(value.outcome);
}

void write_latest_snapshot(const inputnext::connection_snapshot& value,
    input_next_input_snapshot_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    write_domain(inputnext::history_domain{.target = value.target}, out.domain);
    out.sequence = value.sample.revision;
    out.source_timestamp_us = value.sample.source_timestamp_us;
    out.qpc = value.sample.observation_qpc;
    write_gamepad(value.sample.state, out.state);
}

void write_empty_snapshot(const inputnext::connection_target target,
    input_next_input_snapshot_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    write_domain(inputnext::history_domain{.target = target}, out.domain);
}

void write_reader_result(const inputnext::history_batch& value,
    input_next_reader_result_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    out.count = static_cast<std::uint32_t>(value.count);
    if (value.caught_up) {
        out.flags |= INPUT_NEXT_CORE_READER_RESULT_CAUGHT_UP;
    }
    if (value.count != 0U && value.records[0U].baseline) {
        out.flags |= INPUT_NEXT_CORE_READER_RESULT_BASELINE;
    }
    if (value.reset == inputnext::history_reset::history_gap ||
        value.loss != inputnext::history_loss::none) {
        out.flags |= INPUT_NEXT_CORE_READER_RESULT_GAP;
    }
    out.loss_kind = static_cast<std::uint32_t>(value.loss);
    write_domain(value.domain, out.domain);
    out.first_sequence = value.first_sequence;
    out.last_sequence = value.last_sequence;
    out.checkpoint_sequence = value.checkpoint_sequence;
    out.checkpoint_qpc = value.checkpoint_qpc;
    out.lost_count = value.lost_count;
    out.reset_reason = static_cast<std::uint32_t>(value.reset);
}

} // namespace inputnext::runtime_detail::core_projection
