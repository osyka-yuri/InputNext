#include "api.hpp"

#include "api_support.hpp"
#include "core/core.hpp"
#include "effects/effects.hpp"
#include "projection.hpp"
#include "runtime/abi/session_access.hpp"
#include "runtime/session/native_session.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/device_identity.hpp>

#include <cstdint>

namespace inputnext::runtime_detail {

using effects_api_support::commit_output;
using effects_api_support::guarded;
using effects_projection::to_c_status;

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_resolve_target(
    const input_next_device_selector_v1* const selector,
    input_next_device_target_v1* const out_target,
    input_next_effects_capabilities_v1* const out_capabilities) noexcept {
    return guarded([&] {
        if (selector == nullptr || out_target == nullptr || out_capabilities == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (selector->struct_size != sizeof(*selector)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (!effects_projection::valid_output_header(*out_target) ||
            !effects_projection::valid_output_header(*out_capabilities)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto selector_input = *selector;
        inputnext::core::selector selection{};
        if (!effects_projection::parse_selector(selector_input, selection)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::initialize_ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto resolved = session->effects().resolve(selection);
        if (!resolved.has_value()) {
            return to_c_status(resolved.error());
        }

        input_next_device_target_v1 target{};
        input_next_effects_capabilities_v1 capabilities{};
        if (!effects_projection::write_target(resolved->target, target) ||
            !effects_projection::write_capabilities(resolved->value, capabilities)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_target, target);
        commit_output(out_capabilities, capabilities);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_open_context(
    const input_next_effects_open_v1* const request,
    input_next_effects_context_v1* const out_context) noexcept {
    return guarded([&] {
        if (request == nullptr || out_context == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (request->struct_size != sizeof(*request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (!effects_projection::valid_output_header(*out_context)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto request_input = *request;
        connection_target target{};
        if (!effects_projection::parse_open_request(request_input, target)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::initialize_ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto opened = session->effects().open(target);
        if (!opened.has_value()) {
            return to_c_status(opened.error());
        }
        if (!opened->runtime.valid() || opened->id == 0U) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        input_next_effects_context_v1 staged{};
        effects_projection::write_context(*opened, staged);
        commit_output(out_context, staged);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_query_context(
    const input_next_effects_context_v1* const context,
    input_next_effects_context_info_v1* const out_info) noexcept {
    return guarded([&] {
        if (context == nullptr || out_info == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (context->struct_size != sizeof(*context)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (!effects_projection::valid_output_header(*out_info)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto context_input = *context;
        effects::context_handle parsed_context{};
        if (!effects_projection::parse_context(context_input, parsed_context)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto inspected = session->effects().inspect(parsed_context);
        if (!inspected.has_value()) {
            return to_c_status(inspected.error());
        }

        input_next_effects_context_info_v1 staged{};
        if (!effects_projection::write_context_info(*inspected, staged)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_info, staged);
        return inspected->state == effects::context_state::revoked ? INPUT_NEXT_STATUS_REVOKED
                                                                   : INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_close_context(
    const input_next_effects_close_v1* const request,
    input_next_effects_close_receipt_v1* const out_receipt) noexcept {
    return guarded([&] {
        if (request == nullptr || out_receipt == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (request->struct_size != sizeof(*request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (!effects_projection::valid_output_header(*out_receipt)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto request_input = *request;
        effects::context_handle context{};
        std::uint64_t close_request_id{};
        if (!effects_projection::parse_close_request(request_input, context, close_request_id)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto closed = session->effects().close(context, close_request_id);
        if (!closed.has_value()) {
            return to_c_status(closed.error());
        }
        input_next_effects_close_receipt_v1 staged{};
        if (!effects_projection::write_close_receipt(*closed, staged)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_receipt, staged);
        return INPUT_NEXT_STATUS_REVOKED;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_ack_close(
    const input_next_effects_close_receipt_v1* const receipt) noexcept {
    return guarded([&] {
        if (receipt == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (receipt->struct_size != sizeof(*receipt)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto receipt_input = *receipt;
        effects::close_receipt parsed_receipt{};
        if (!effects_projection::parse_close_receipt(receipt_input, parsed_receipt)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::retained);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        return to_c_status(session->effects().acknowledge(parsed_receipt));
    });
}

} // namespace inputnext::runtime_detail
