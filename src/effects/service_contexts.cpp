#include "implementation.hpp"

#include "effects.hpp"
#include "error_mapping.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/error.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <cstddef>
#include <expected>
#include <new>
#include <stdexcept>

namespace inputnext::effects {

service::implementation::implementation(service_dependencies supplied)
    : dependencies(supplied),
      identity(supplied.identity) {
    if (!identity.valid()) {
        throw std::invalid_argument{"Effects service requires a valid runtime identity"};
    }
}

context_record* service::implementation::find(const context_handle context) noexcept {
    for (auto& record : contexts) {
        if ((record.phase == context_phase::open ||
                record.phase == context_phase::closed_retained ||
                record.phase == context_phase::acknowledged_tombstone) &&
            record.external_context == context) {
            return &record;
        }
    }
    return nullptr;
}

result<context_record*> service::implementation::find_open(const context_handle context) noexcept {
    if (!context.runtime.valid() || context.runtime != identity || context.id == 0U) {
        return std::unexpected{status::stale_context};
    }
    auto* record = find(context);
    if (record == nullptr || record->phase == context_phase::acknowledged_tombstone) {
        return std::unexpected{status::stale_context};
    }
    if (record->phase != context_phase::open) {
        return std::unexpected{status::revoked};
    }
    return record;
}

bool service::implementation::target_retired(const connection_target target) const noexcept {
    const auto pin = dependencies.registry.capture(target);
    return !pin.has_value() && pin.error() == inputnext::error::not_connected;
}

std::size_t service::implementation::occupied_contexts() const noexcept {
    std::size_t result = 0U;
    for (const auto& record : contexts) {
        result += static_cast<std::size_t>(record.phase == context_phase::reserved ||
                                           record.phase == context_phase::open ||
                                           record.phase == context_phase::closed_retained);
    }
    return result;
}

std::size_t service::implementation::pending_cleanup() const noexcept {
    std::size_t result = 0U;
    for (const auto& record : contexts) {
        result += static_cast<std::size_t>(record.cleanup_selected && !record.native_released);
    }
    return result;
}

void service::implementation::retain_native_close(context_record& record,
    const output::close_receipt receipt) noexcept {
    if (record.external_close.has_value()) {
        return;
    }
    record.native_close = receipt;
    record.native_released = false;
    record.phase = context_phase::closed_retained;
    record.external_close = close_receipt{.disposition = close_disposition::normal,
        .context = record.external_context,
        .target = record.target,
        .close_request_id = record.external_context.id,
        .publication = record.close_publication};
    record.body_identity.reset();
}

void service::implementation::retain_retired_close(context_record& record) noexcept {
    if (record.external_close.has_value()) {
        record.native_released = true;
        record.cleanup_selected = false;
        return;
    }
    record.native_close.reset();
    record.native_released = true;
    record.phase = context_phase::closed_retained;
    record.external_close = close_receipt{.disposition = close_disposition::connection_retired,
        .context = record.external_context,
        .target = record.target,
        .close_request_id = record.external_context.id,
        .publication = record.close_publication};
    record.body_identity.reset();
    record.cleanup_selected = false;
}

status service::implementation::close_record(context_record& record) noexcept {
    if (record.external_close.has_value()) {
        return status::ok;
    }
    if (record.phase != context_phase::open) {
        return status::stale_context;
    }

    try {
        const auto closed = dependencies.authority.close(record.native_context,
            record.native_context.identity.id.value);
        if (closed.has_value()) {
            if (closed->identity != record.native_context.identity ||
                closed->close_request_id != record.native_context.identity.id) {
                return status::io_failed;
            }
            retain_native_close(record, *closed);
            return status::ok;
        }
        if (closed.error() == output::error::stale_context && target_retired(record.target)) {
            retain_retired_close(record);
            return status::ok;
        }
        return map_output_error(closed.error());
    } catch (const std::bad_alloc&) {
        return status::capacity_exceeded;
    } catch (...) {
        return status::io_failed;
    }
}

status service::implementation::release_native(context_record& record) noexcept {
    if (record.native_released) {
        return status::ok;
    }
    if (!record.native_close.has_value()) {
        if (target_retired(record.target)) {
            record.native_released = true;
            record.cleanup_selected = false;
            return status::ok;
        }
        record.cleanup_selected = true;
        return status::stale_context;
    }

    try {
        const auto acknowledged = dependencies.authority.acknowledge(*record.native_close);
        if (acknowledged.has_value()) {
            record.native_released = true;
            record.cleanup_selected = false;
            return status::ok;
        }
        if (acknowledged.error() == output::error::stale_context && target_retired(record.target)) {
            record.native_released = true;
            record.cleanup_selected = false;
            return status::ok;
        }
        record.cleanup_selected = true;
        return map_output_error(acknowledged.error());
    } catch (const std::bad_alloc&) {
        record.cleanup_selected = true;
        return status::capacity_exceeded;
    } catch (...) {
        record.cleanup_selected = true;
        return status::io_failed;
    }
}

} // namespace inputnext::effects
