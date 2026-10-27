#pragma once

#include <input_next_core.h>
#include <input_next_effects.h>

namespace inputnext::runtime_detail {

[[nodiscard]] const input_next_effects_api_v1* effects_api_v1() noexcept;

[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_resolve_target(
    const input_next_device_selector_v1* selector,
    input_next_device_target_v1* out_target,
    input_next_effects_capabilities_v1* out_capabilities) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_open_context(
    const input_next_effects_open_v1* request,
    input_next_effects_context_v1* out_context) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_query_context(
    const input_next_effects_context_v1* context,
    input_next_effects_context_info_v1* out_info) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_submit(
    const input_next_effects_request_v1* request,
    input_next_effects_receipt_v1* out_receipt) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_clear(
    const input_next_effects_clear_v1* request) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_submit_body(
    const input_next_effects_body_request_v1* request,
    input_next_effects_body_receipt_v1* out_receipt) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_clear_body(
    const input_next_effects_body_clear_v1* request) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_close_context(
    const input_next_effects_close_v1* request,
    input_next_effects_close_receipt_v1* out_receipt) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_ack_close(
    const input_next_effects_close_receipt_v1* receipt) noexcept;
[[nodiscard]] input_next_status INPUT_NEXT_EFFECTS_CALL api_query_progress(
    const input_next_effects_progress_query_v1* query,
    input_next_effects_progress_v1* out_progress) noexcept;

} // namespace inputnext::runtime_detail
