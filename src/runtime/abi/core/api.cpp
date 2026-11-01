#include "api.hpp"

#include "core/core.hpp"
#include "effects/effects.hpp"
#include "projection.hpp"
#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/abi/session_access.hpp"
#include "runtime/bootstrap/query_front.hpp"
#include "runtime/session/native_session.hpp"
#include <input_next_core.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace inputnext::runtime_detail {
namespace {

using inputnext::connection_target;
using inputnext::core::reader_focus;
using inputnext::core::selector;
using inputnext::core::status;

static_assert(inputnext::core::device_capacity == 16U);
static_assert(inputnext::core::reader_capacity == 28U);
static_assert(inputnext::history_batch_capacity == 32U);

template <typename Operation>
[[nodiscard]] input_next_status guarded(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return INPUT_NEXT_STATUS_CAPACITY_EXCEEDED;
    } catch (...) {
        return INPUT_NEXT_STATUS_IO_FAILED;
    }
}

[[nodiscard]] bool valid_reader_handle(const input_next_reader_v1& value,
    inputnext::core::reader_handle& out) noexcept {
    out = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U || value.id == 0U) {
        return false;
    }
    std::memcpy(out.runtime.bytes.data(), value.runtime_id, out.runtime.bytes.size());
    out.id = value.id;
    return out.runtime.valid();
}

void write_reader_handle(const inputnext::core::reader_handle& value,
    input_next_reader_v1& out) noexcept {
    const auto descriptor_size = out.struct_size;
    out = {};
    out.struct_size = descriptor_size;
    std::memcpy(out.runtime_id, value.runtime.bytes.data(), value.runtime.bytes.size());
    out.id = value.id;
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL get_runtime_info(
    input_next_runtime_info_v1* const out_info) noexcept {
    if (out_info == nullptr || out_info->struct_size != sizeof(*out_info) ||
        out_info->flags != 0U) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const auto descriptor_size = out_info->struct_size;
    *out_info = {};
    out_info->struct_size = descriptor_size;

    return guarded([out_info] {
        auto* const session = session_for(session_access::initialize_retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto info = session->core().info();
        if (!info.runtime.valid() || info.qpc_frequency == 0U) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        out_info->abi_version = INPUT_NEXT_CORE_ABI_VERSION;
        std::memcpy(out_info->runtime_id, info.runtime.bytes.data(), info.runtime.bytes.size());
        out_info->qpc_frequency = info.qpc_frequency;
        out_info->max_devices = static_cast<std::uint32_t>(inputnext::core::device_capacity);
        out_info->max_readers = static_cast<std::uint32_t>(inputnext::core::reader_capacity);
        out_info->max_effect_contexts = static_cast<std::uint32_t>(effects::context_capacity);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL enumerate_devices(
    input_next_device_info_v1* const out_devices,
    const std::uint32_t capacity,
    std::uint32_t* const out_count,
    std::uint64_t* const out_discovery_revision) noexcept {
    if (out_count == nullptr || out_discovery_revision == nullptr ||
        (capacity != 0U && out_devices == nullptr)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const auto writable_capacity =
        std::min<std::size_t>(capacity, inputnext::core::device_capacity);
    for (std::size_t index = 0U; index < writable_capacity; ++index) {
        if (out_devices[index].struct_size != sizeof(input_next_device_info_v1) ||
            out_devices[index].reserved != 0U) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
    }
    *out_count = 0U;
    *out_discovery_revision = 0U;

    return guarded([=] {
        auto* const session = session_for(session_access::initialize_ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        auto result = session->core().enumerate();
        if (!result.has_value()) {
            return core_projection::to_c_status(result.error());
        }
        const auto& catalogue = *result;
        if (catalogue.count > catalogue.entries.size() || catalogue.revision == 0U) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        *out_count = static_cast<std::uint32_t>(catalogue.count);
        *out_discovery_revision = catalogue.revision;
        if (catalogue.count > capacity) {
            return INPUT_NEXT_STATUS_BUFFER_TOO_SMALL;
        }

        for (std::size_t index = 0U; index < catalogue.count; ++index) {
            if (!core_projection::valid_device(catalogue.entries[index])) {
                *out_count = 0U;
                *out_discovery_revision = 0U;
                return INPUT_NEXT_STATUS_UNAVAILABLE;
            }
        }
        for (std::size_t index = 0U; index < catalogue.count; ++index) {
            core_projection::write_device(catalogue.entries[index], out_devices[index]);
        }
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL resolve_target(
    const input_next_device_selector_v1* const in_selector,
    input_next_device_target_v1* const out_target) noexcept {
    if (in_selector == nullptr || out_target == nullptr ||
        out_target->struct_size != sizeof(*out_target) || out_target->reserved != 0U) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const input_next_device_selector_v1 selector_copy = *in_selector;
    const auto descriptor_size = out_target->struct_size;
    selector selection{};
    if (!core_projection::parse_selector(selector_copy, selection)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    *out_target = {};
    out_target->struct_size = descriptor_size;

    return guarded([&selection, out_target] {
        auto* const session = session_for(session_access::initialize_ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        auto result = session->core().resolve(selection);
        if (!result.has_value()) {
            return core_projection::to_c_status(result.error());
        }
        if (!result->valid()) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        core_projection::write_target(*result, *out_target);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL read_latest(
    const input_next_device_target_v1* const in_target,
    input_next_input_snapshot_v1* const out_snapshot) noexcept {
    if (in_target == nullptr || out_snapshot == nullptr ||
        out_snapshot->struct_size != sizeof(*out_snapshot) || out_snapshot->reserved != 0U) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const auto target_copy = *in_target;
    connection_target target{};
    if (!core_projection::parse_target(target_copy, target)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const auto descriptor_size = out_snapshot->struct_size;
    *out_snapshot = {};
    out_snapshot->struct_size = descriptor_size;

    return guarded([target, out_snapshot] {
        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        auto result = session->core().read_latest(target);
        if (!result.has_value()) {
            if (result.error() == status::no_data) {
                core_projection::write_empty_snapshot(target, *out_snapshot);
                return INPUT_NEXT_STATUS_NO_DATA;
            }
            return core_projection::to_c_status(result.error());
        }
        if (result->target != target || result->sample.revision == 0U ||
            !inputnext::is_valid_gamepad_state(result->sample.state)) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        core_projection::write_latest_snapshot(*result, *out_snapshot);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL open_reader(
    const input_next_reader_request_v1* const in_request,
    input_next_reader_v1* const out_reader) noexcept {
    if (in_request == nullptr || out_reader == nullptr ||
        out_reader->struct_size != sizeof(*out_reader) || out_reader->reserved != 0U) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const input_next_reader_request_v1 request = *in_request;
    const auto descriptor_size = out_reader->struct_size;
    connection_target target{};
    if (request.struct_size != sizeof(request) || request.flags != 0U || request.reserved != 0U ||
        !core_projection::parse_target(request.target, target)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    reader_focus focus{};
    if (request.focus_mode == INPUT_NEXT_CORE_READER_FOREGROUND_PROCESS) {
        focus = reader_focus::foreground_process;
    } else if (request.focus_mode == INPUT_NEXT_CORE_READER_BACKGROUND) {
        focus = reader_focus::background;
    } else {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    *out_reader = {};
    out_reader->struct_size = descriptor_size;

    return guarded([target, focus, out_reader] {
        auto* const session = session_for(session_access::initialize_ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        auto result = session->core().open_reader(target, focus);
        if (!result.has_value()) {
            return core_projection::to_c_status(result.error());
        }
        if (!result->runtime.valid() || result->id == 0U) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        write_reader_handle(*result, *out_reader);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL read_events(
    const input_next_reader_v1* const in_reader,
    input_next_input_snapshot_v1* const out_snapshots,
    const std::uint32_t capacity,
    input_next_reader_result_v1* const out_result) noexcept {
    if (in_reader == nullptr || out_result == nullptr || capacity == 0U ||
        capacity > inputnext::history_batch_capacity || out_snapshots == nullptr ||
        out_result->struct_size != sizeof(*out_result) || out_result->reserved != 0U) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    const input_next_reader_v1 reader_copy = *in_reader;
    inputnext::core::reader_handle reader{};
    if (!valid_reader_handle(reader_copy, reader)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    for (std::size_t index = 0U; index < capacity; ++index) {
        if (out_snapshots[index].struct_size != sizeof(input_next_input_snapshot_v1) ||
            out_snapshots[index].reserved != 0U) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
    }
    const auto descriptor_size = out_result->struct_size;
    *out_result = {};
    out_result->struct_size = descriptor_size;

    return guarded([reader, out_snapshots, capacity, out_result] {
        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto result = session->core().read_events(reader, capacity);
        return core_projection::write_reader_batch(result, out_snapshots, capacity, *out_result);
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_CORE_CALL close_reader(
    const input_next_reader_v1* const in_reader) noexcept {
    if (in_reader == nullptr) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }
    inputnext::core::reader_handle reader{};
    if (!valid_reader_handle(*in_reader, reader)) {
        return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
    }

    return guarded([reader] {
        auto* const session = session_for(session_access::retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        return core_projection::to_c_status(session->core().close_reader(reader));
    });
}

constexpr input_next_core_api_v1 core_api_table{
    .struct_size = sizeof(input_next_core_api_v1),
    .abi_version = INPUT_NEXT_CORE_ABI_VERSION,
    .get_runtime_info = &get_runtime_info,
    .enumerate_devices = &enumerate_devices,
    .resolve_target = &resolve_target,
    .read_latest = &read_latest,
    .open_reader = &open_reader,
    .read_events = &read_events,
    .close_reader = &close_reader,
};

static_assert(core_api_table.struct_size == sizeof(input_next_core_api_v1));
static_assert(core_api_table.abi_version == INPUT_NEXT_CORE_ABI_VERSION);
static_assert(core_api_table.get_runtime_info != nullptr &&
              core_api_table.enumerate_devices != nullptr &&
              core_api_table.resolve_target != nullptr && core_api_table.read_latest != nullptr &&
              core_api_table.open_reader != nullptr && core_api_table.read_events != nullptr &&
              core_api_table.close_reader != nullptr);

} // namespace

const input_next_core_api_v1* core_api_v1() noexcept {
    return &core_api_table;
}

} // namespace inputnext::runtime_detail

extern "C" const input_next_core_api_v1* INPUT_NEXT_CORE_CALL input_next_runtime_query_core_api(
    const std::uint32_t requested_abi) noexcept {
    if (requested_abi != INPUT_NEXT_CORE_ABI_VERSION) {
        return nullptr;
    }
    const auto* const private_api = inputnext::runtime_detail::query_runtime_api(
        inputnext::runtime_detail::runtime_query_front(),
        1U);
    return private_api != nullptr ? inputnext::runtime_detail::core_api_v1() : nullptr;
}
