#include "core_layout_assertions.h"

#include <cstdint>
#include <type_traits>

using runtime_info_callback = input_next_status(INPUT_NEXT_CORE_CALL*)(
    input_next_runtime_info_v1*) noexcept;
using enumerate_callback = input_next_status(
    INPUT_NEXT_CORE_CALL*)(input_next_device_info_v1*, uint32_t, uint32_t*, uint64_t*) noexcept;
using resolve_callback = input_next_status(
    INPUT_NEXT_CORE_CALL*)(const input_next_device_selector_v1*,
    input_next_device_target_v1*) noexcept;
using latest_callback = input_next_status(INPUT_NEXT_CORE_CALL*)(const input_next_device_target_v1*,
    input_next_input_snapshot_v1*) noexcept;
using open_callback = input_next_status(
    INPUT_NEXT_CORE_CALL*)(const input_next_reader_request_v1*, input_next_reader_v1*) noexcept;
using events_callback = input_next_status(INPUT_NEXT_CORE_CALL*)(const input_next_reader_v1*,
    input_next_input_snapshot_v1*,
    uint32_t,
    input_next_reader_result_v1*) noexcept;
using close_callback = input_next_status(INPUT_NEXT_CORE_CALL*)(
    const input_next_reader_v1*) noexcept;

static_assert(
    std::is_same_v<decltype(input_next_core_api_v1::get_runtime_info), runtime_info_callback>);
static_assert(
    std::is_same_v<decltype(input_next_core_api_v1::enumerate_devices), enumerate_callback>);
static_assert(std::is_same_v<decltype(input_next_core_api_v1::resolve_target), resolve_callback>);
static_assert(std::is_same_v<decltype(input_next_core_api_v1::read_latest), latest_callback>);
static_assert(std::is_same_v<decltype(input_next_core_api_v1::open_reader), open_callback>);
static_assert(std::is_same_v<decltype(input_next_core_api_v1::read_events), events_callback>);
static_assert(std::is_same_v<decltype(input_next_core_api_v1::close_reader), close_callback>);
static_assert(std::is_same_v<decltype(&input_next_runtime_query_core_api),
    pfn_input_next_runtime_query_core_api>);
static_assert(std::is_standard_layout_v<input_next_core_api_v1>);
static_assert(std::is_trivially_copyable_v<input_next_input_snapshot_v1>);

int main() {
    return 0;
}
