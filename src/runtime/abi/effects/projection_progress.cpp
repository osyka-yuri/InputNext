#include "projection.hpp"

#include "effects/effects.hpp"
#include "runtime/abi/core/projection.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/device_identity.hpp>

#include <cstdint>

namespace inputnext::runtime_detail::effects_projection {
namespace {
[[nodiscard]] bool parse_completion_status(const effects::status value,
    input_next_status& out) noexcept {
    switch (value) {
    case effects::status::ok:
    case effects::status::invalid_argument:
    case effects::status::unavailable:
    case effects::status::stale_target:
    case effects::status::unsupported_target:
    case effects::status::unsupported_channel:
    case effects::status::scheduler_unavailable:
    case effects::status::busy:
    case effects::status::stale_lease:
    case effects::status::capacity_exceeded:
    case effects::status::stale_context:
    case effects::status::stale_effect:
    case effects::status::stale_sequence:
    case effects::status::source_retired:
    case effects::status::unsupported:
    case effects::status::io_failed:
    case effects::status::revoked:
    case effects::status::pending:
    case effects::status::no_data:
    case effects::status::not_admitted:
        out = to_c_status(value);
        return true;
    }
    return false;
}

[[nodiscard]] bool write_stage(const effects::progress_stage value, std::uint32_t& out) noexcept {
    switch (value) {
    case effects::progress_stage::dispatch_pending:
        out = INPUT_NEXT_EFFECTS_PROGRESS_DISPATCH_PENDING;
        return true;
    case effects::progress_stage::current_completed:
        out = INPUT_NEXT_EFFECTS_PROGRESS_CURRENT_COMPLETED;
        return true;
    case effects::progress_stage::superseded:
        out = INPUT_NEXT_EFFECTS_PROGRESS_SUPERSEDED;
        return true;
    case effects::progress_stage::connection_retired:
        out = INPUT_NEXT_EFFECTS_PROGRESS_CONNECTION_RETIRED;
        return true;
    case effects::progress_stage::unknown:
        out = INPUT_NEXT_EFFECTS_PROGRESS_UNKNOWN;
        return true;
    case effects::progress_stage::not_deliverable:
        out = INPUT_NEXT_EFFECTS_PROGRESS_NOT_DELIVERABLE;
        return true;
    }
    return false;
}

[[nodiscard]] bool write_completion_class(const effects::completion_class value,
    std::uint32_t& out) noexcept {
    switch (value) {
    case effects::completion_class::none:
        out = INPUT_NEXT_EFFECTS_COMPLETION_NONE;
        return true;
    case effects::completion_class::setter_returned_current:
        out = INPUT_NEXT_EFFECTS_COMPLETION_SETTER_RETURNED_CURRENT;
        return true;
    case effects::completion_class::raw_output_accepted_current:
        out = INPUT_NEXT_EFFECTS_COMPLETION_RAW_OUTPUT_ACCEPTED_CURRENT;
        return true;
    }
    return false;
}

} // namespace

bool parse_close_request(const input_next_effects_close_v1& value,
    effects::context_handle& context,
    std::uint64_t& close_request_id) noexcept {
    context = {};
    close_request_id = 0U;
    if (value.struct_size != sizeof(value) || value.reserved != 0U ||
        !parse_context(value.context, context) || value.close_request_id == 0U ||
        value.close_request_id != context.id) {
        return false;
    }
    close_request_id = value.close_request_id;
    return true;
}

bool parse_close_receipt(const input_next_effects_close_receipt_v1& value,
    effects::close_receipt& out) noexcept {
    out = {};
    if (value.struct_size != sizeof(value) || value.publication_revision == 0U ||
        value.close_request_id == 0U) {
        return false;
    }
    effects::context_handle context{};
    connection_target target{};
    if (!parse_context(value.context, context) ||
        !core_projection::parse_target(value.target, target) ||
        value.close_request_id != context.id) {
        return false;
    }

    effects::close_disposition disposition{};
    switch (value.disposition) {
    case INPUT_NEXT_EFFECTS_CLOSE_NORMAL:
        disposition = effects::close_disposition::normal;
        break;
    case INPUT_NEXT_EFFECTS_CLOSE_CONNECTION_RETIRED:
        disposition = effects::close_disposition::connection_retired;
        break;
    default:
        return false;
    }
    out = {.disposition = disposition,
        .context = context,
        .target = target,
        .close_request_id = value.close_request_id,
        .publication = {.value = value.publication_revision}};
    return true;
}

bool parse_progress_query(const input_next_effects_progress_query_v1& value,
    connection_target& target,
    effects::publication_revision& publication) noexcept {
    target = {};
    publication = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U ||
        value.publication_revision == 0U || !core_projection::parse_target(value.target, target)) {
        return false;
    }
    publication.value = value.publication_revision;
    return true;
}

bool write_close_receipt(const effects::close_receipt& value,
    input_next_effects_close_receipt_v1& out) noexcept {
    if (!value.context.runtime.valid() || value.context.id == 0U || !value.target.valid() ||
        value.close_request_id != value.context.id || value.publication.value == 0U) {
        return false;
    }
    std::uint32_t disposition{};
    switch (value.disposition) {
    case effects::close_disposition::normal:
        disposition = INPUT_NEXT_EFFECTS_CLOSE_NORMAL;
        break;
    case effects::close_disposition::connection_retired:
        disposition = INPUT_NEXT_EFFECTS_CLOSE_CONNECTION_RETIRED;
        break;
    default:
        return false;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.disposition = disposition;
    write_context(value.context, out.context);
    if (!write_target(value.target, out.target)) {
        return false;
    }
    out.close_request_id = value.close_request_id;
    out.publication_revision = value.publication.value;
    return true;
}

bool write_progress(const effects::progress& value, input_next_effects_progress_v1& out) noexcept {
    if (!value.target.valid()) {
        return false;
    }
    std::uint32_t stage{};
    std::uint32_t completion{};
    input_next_status completion_status{};
    if (!write_stage(value.stage, stage) || !write_completion_class(value.completion, completion) ||
        !parse_completion_status(value.completion_status, completion_status)) {
        return false;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.stage = stage;
    out.completion_class = completion;
    out.completion_status = completion_status;
    if (!write_target(value.target, out.target)) {
        return false;
    }
    out.requested_publication_revision = value.requested.value;
    out.current_publication_revision = value.current.value;
    out.attempt_serial = value.attempt_serial;
    return true;
}

} // namespace inputnext::runtime_detail::effects_projection
