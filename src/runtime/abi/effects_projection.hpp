#pragma once

#include "core/core.hpp"
#include "effects/effects.hpp"
#include <input_next_core.h>
#include <input_next_effects.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include <cstdint>

namespace inputnext::runtime_detail::effects_projection {

[[nodiscard]] input_next_status to_c_status(effects::status value) noexcept;

[[nodiscard]] bool parse_context(const input_next_effects_context_v1& value,
    effects::context_handle& out) noexcept;
void write_context(const effects::context_handle& value,
    input_next_effects_context_v1& out) noexcept;

[[nodiscard]] bool valid_output_header(const input_next_device_target_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_capabilities_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_context_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_context_info_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_receipt_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_body_receipt_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_close_receipt_v1& value) noexcept;
[[nodiscard]] bool valid_output_header(const input_next_effects_progress_v1& value) noexcept;

[[nodiscard]] bool parse_open_request(const input_next_effects_open_v1& value,
    connection_target& out) noexcept;
[[nodiscard]] bool parse_selector(const input_next_device_selector_v1& value,
    core::selector& out) noexcept;
[[nodiscard]] bool parse_submit_request(const input_next_effects_request_v1& value,
    effects::context_handle& context,
    output::submit_request& out) noexcept;
[[nodiscard]] bool parse_clear_request(const input_next_effects_clear_v1& value,
    effects::context_handle& context,
    output::clear_request& out) noexcept;
[[nodiscard]] bool parse_body_request(const input_next_effects_body_request_v1& value,
    effects::body_request& out) noexcept;
[[nodiscard]] bool parse_body_clear_request(const input_next_effects_body_clear_v1& value,
    effects::body_clear_request& out) noexcept;
[[nodiscard]] bool parse_close_request(const input_next_effects_close_v1& value,
    effects::context_handle& context,
    std::uint64_t& close_request_id) noexcept;
[[nodiscard]] bool parse_close_receipt(const input_next_effects_close_receipt_v1& value,
    effects::close_receipt& out) noexcept;
[[nodiscard]] bool parse_progress_query(const input_next_effects_progress_query_v1& value,
    connection_target& target,
    effects::publication_revision& publication) noexcept;

[[nodiscard]] bool write_target(const connection_target& value,
    input_next_device_target_v1& out) noexcept;
[[nodiscard]] bool write_capabilities(const effects::capabilities& value,
    input_next_effects_capabilities_v1& out) noexcept;
[[nodiscard]] bool write_context_info(const effects::context_info& value,
    input_next_effects_context_info_v1& out) noexcept;
[[nodiscard]] bool write_discrete_receipt(const effects::discrete_receipt& value,
    input_next_effects_receipt_v1& out) noexcept;
[[nodiscard]] bool write_body_receipt(const effects::body_receipt& value,
    input_next_effects_body_receipt_v1& out) noexcept;
[[nodiscard]] bool write_close_receipt(const effects::close_receipt& value,
    input_next_effects_close_receipt_v1& out) noexcept;
[[nodiscard]] bool write_progress(const effects::progress& value,
    input_next_effects_progress_v1& out) noexcept;

} // namespace inputnext::runtime_detail::effects_projection
