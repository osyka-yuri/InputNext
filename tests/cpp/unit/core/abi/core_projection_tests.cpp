#include "runtime/abi/core_api_projection.hpp"

#include "core/core.hpp"
#include <input_next_core.h>
#include <inputnext/device_metadata.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_CASE("core.abi.projection_initializes_nested_descriptors_and_unknown_transport",
    "[unit][core][abi]") {
    inputnext::core::device_record source{};
    source.target.runtime.bytes[0U] = std::byte{1U};
    source.target.device.value = 2U;
    source.target.connection.value = 3U;
    source.transport = inputnext::physical_transport::other;

    input_next_device_info_v1 projected{};
    projected.struct_size = sizeof(projected);
    inputnext::runtime_detail::core_projection::write_device(source, projected);

    CHECK(projected.struct_size == sizeof(projected));
    CHECK(projected.target.struct_size == sizeof(projected.target));
    CHECK(projected.target.reserved == 0U);
    CHECK(projected.target.runtime_id[0U] == 1U);
    CHECK(projected.target.device_key == 2U);
    CHECK(projected.target.connection_id == 3U);
    CHECK(projected.input_profile == INPUT_NEXT_CORE_INPUT_PROFILE_GAMEPAD);
    CHECK(projected.transport == INPUT_NEXT_CORE_TRANSPORT_UNKNOWN);
    CHECK(projected.identity_quality == INPUT_NEXT_CORE_IDENTITY_NONE);
    CHECK(projected.flags == 0U);
    CHECK(projected.reserved == 0U);
}

TEST_CASE("core.abi.projection_preserves_error_batch_prefix_and_recovery_metadata",
    "[unit][core][abi]") {
    inputnext::core::reader_result source{};
    source.outcome = inputnext::core::status::io_failed;
    source.batch.domain.target.runtime.bytes[0U] = std::byte{1U};
    source.batch.domain.target.device.value = 2U;
    source.batch.domain.target.connection.value = 3U;
    source.batch.domain.stream = 4U;
    source.batch.records[0U].sequence = 8U;
    source.batch.records[0U].baseline = true;
    source.batch.records[0U].sample.source_timestamp_us = 9U;
    source.batch.records[0U].sample.effective_qpc = 10U;
    source.batch.count = 1U;
    source.batch.loss = inputnext::history_loss::unknown;
    source.batch.reset = inputnext::history_reset::history_gap;
    source.batch.checkpoint_sequence = 12U;
    source.batch.checkpoint_qpc = 13U;

    input_next_input_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    input_next_reader_result_v1 result{};
    result.struct_size = sizeof(result);
    const auto status = inputnext::runtime_detail::core_projection::write_reader_batch(source,
        &snapshot,
        1U,
        result);

    CHECK(status == INPUT_NEXT_STATUS_IO_FAILED);
    CHECK(result.count == 1U);
    CHECK((result.flags & INPUT_NEXT_CORE_READER_RESULT_GAP) != 0U);
    CHECK((result.flags & INPUT_NEXT_CORE_READER_RESULT_BASELINE) != 0U);
    CHECK(result.loss_kind == INPUT_NEXT_CORE_READER_LOSS_UNKNOWN);
    CHECK(result.reset_reason == INPUT_NEXT_CORE_READER_RESET_HISTORY_GAP);
    CHECK(result.checkpoint_sequence == 12U);
    CHECK(result.checkpoint_qpc == 13U);
    CHECK(snapshot.sequence == 8U);
    CHECK(snapshot.source_timestamp_us == 9U);
    CHECK(snapshot.qpc == 10U);
    CHECK((snapshot.flags & INPUT_NEXT_CORE_SNAPSHOT_BASELINE) != 0U);
}

TEST_CASE("core.abi.projection_reports_short_buffer_without_writing_snapshots",
    "[unit][core][abi]") {
    inputnext::core::reader_result source{};
    source.outcome = inputnext::core::status::buffer_too_small;
    source.batch.domain.target.runtime.bytes[0U] = std::byte{1U};
    source.batch.domain.target.device.value = 2U;
    source.batch.domain.target.connection.value = 3U;
    source.batch.domain.stream = 4U;
    source.batch.count = 2U;
    source.batch.first_sequence = 8U;
    source.batch.last_sequence = 9U;

    input_next_input_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.flags = 0xA5U;
    input_next_reader_result_v1 result{};
    result.struct_size = sizeof(result);
    const auto status = inputnext::runtime_detail::core_projection::write_reader_batch(source,
        &snapshot,
        1U,
        result);

    CHECK(status == INPUT_NEXT_STATUS_BUFFER_TOO_SMALL);
    CHECK(result.count == 2U);
    CHECK(result.first_sequence == 8U);
    CHECK(result.last_sequence == 9U);
    CHECK(snapshot.flags == 0xA5U);
}
