#pragma once

#include "core/core.hpp"
#include <input_next_core.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <cstddef>
#include <cstdint>

namespace inputnext::runtime_detail::core_projection {

[[nodiscard]] input_next_status to_c_status(inputnext::core::status value) noexcept;
[[nodiscard]] bool parse_target(const input_next_device_target_v1& value,
    inputnext::connection_target& out) noexcept;
[[nodiscard]] bool target_is_zero(const input_next_device_target_v1& value) noexcept;
[[nodiscard]] bool is_zero_bytes(const std::uint8_t* value, std::size_t size) noexcept;
void write_target(const inputnext::connection_target& value,
    input_next_device_target_v1& out) noexcept;
void write_domain(const inputnext::history_domain& value, input_next_input_domain_v1& out) noexcept;
void write_device(const inputnext::core::device_record& value,
    input_next_device_info_v1& out) noexcept;
[[nodiscard]] bool valid_device(const inputnext::core::device_record& value) noexcept;
void write_snapshot(const inputnext::history_record& value,
    inputnext::history_domain domain,
    input_next_input_snapshot_v1& out) noexcept;
[[nodiscard]] input_next_status write_reader_batch(const inputnext::core::reader_result& value,
    input_next_input_snapshot_v1* out_snapshots,
    std::size_t capacity,
    input_next_reader_result_v1& out_result) noexcept;
void write_latest_snapshot(const inputnext::connection_snapshot& value,
    input_next_input_snapshot_v1& out) noexcept;
void write_empty_snapshot(inputnext::connection_target target,
    input_next_input_snapshot_v1& out) noexcept;
void write_reader_result(const inputnext::history_batch& value,
    input_next_reader_result_v1& out) noexcept;

} // namespace inputnext::runtime_detail::core_projection
