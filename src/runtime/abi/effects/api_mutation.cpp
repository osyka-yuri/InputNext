#include "api.hpp"

#include "api_support.hpp"
#include "effects/effects.hpp"
#include "projection.hpp"
#include "runtime/abi/session_access.hpp"
#include "runtime/session/native_session.hpp"

#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/output.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>

namespace inputnext::runtime_detail {

using effects_api_support::commit_output;
using effects_api_support::guarded;
using effects_projection::to_c_status;

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_submit(
    const input_next_effects_request_v1* const request,
    input_next_effects_receipt_v1* const out_receipt) noexcept {
    const auto entered_at = std::chrono::steady_clock::now();
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
        output::submit_request parsed_request{};
        if (!effects_projection::parse_submit_request(request_input, context, parsed_request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto window = output::make_finite_submission_window(entered_at, parsed_request.ttl);
        if (!window.has_value()) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto submitted = session->effects().submit(context, parsed_request, *window);
        if (!submitted.has_value()) {
            return to_c_status(submitted.error());
        }
        input_next_effects_receipt_v1 staged{};
        if (!effects_projection::write_discrete_receipt(*submitted, staged)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_receipt, staged);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_clear(
    const input_next_effects_clear_v1* const request) noexcept {
    return guarded([&] {
        if (request == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (request->struct_size != sizeof(*request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto request_input = *request;
        effects::context_handle context{};
        output::clear_request parsed_request{};
        if (!effects_projection::parse_clear_request(request_input, context, parsed_request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        return to_c_status(session->effects().clear(context, parsed_request));
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_submit_body(
    const input_next_effects_body_request_v1* const request,
    input_next_effects_body_receipt_v1* const out_receipt) noexcept {
    const auto entered_at = std::chrono::steady_clock::now();
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
        effects::body_request parsed_request{};
        if (!effects_projection::parse_body_request(request_input, parsed_request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto window = output::make_finite_submission_window(entered_at, parsed_request.ttl);
        if (!window.has_value()) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        const auto submitted = session->effects().submit_body(parsed_request, *window);
        if (!submitted.has_value()) {
            return to_c_status(submitted.error());
        }
        input_next_effects_body_receipt_v1 staged{};
        if (!effects_projection::write_body_receipt(*submitted, staged)) {
            return INPUT_NEXT_STATUS_IO_FAILED;
        }
        commit_output(out_receipt, staged);
        return INPUT_NEXT_STATUS_OK;
    });
}

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_clear_body(
    const input_next_effects_body_clear_v1* const request) noexcept {
    return guarded([&] {
        if (request == nullptr) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        if (request->struct_size != sizeof(*request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }
        const auto request_input = *request;
        effects::body_clear_request parsed_request{};
        if (!effects_projection::parse_body_clear_request(request_input, parsed_request)) {
            return INPUT_NEXT_STATUS_INVALID_ARGUMENT;
        }

        auto* const session = session_for(session_access::ready);
        if (session == nullptr) {
            return INPUT_NEXT_STATUS_UNAVAILABLE;
        }
        return to_c_status(session->effects().clear_body(parsed_request));
    });
}

} // namespace inputnext::runtime_detail
