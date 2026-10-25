#include "core_reader.h"

#include <input_next_core.h>

#include <stdio.h>

#include <stdint.h>

input_next_status inputnext_example_read_once(const input_next_core_api_v1* api) {
    input_next_runtime_info_v1 info = {0};
    info.struct_size = sizeof(info);
    input_next_status status = api->get_runtime_info(&info);
    if (status != INPUT_NEXT_STATUS_OK) {
        return status;
    }

    input_next_device_info_v1 devices[16] = {0};
    for (uint32_t index = 0; index < 16; ++index) {
        devices[index].struct_size = sizeof(devices[index]);
    }
    uint32_t count = 0;
    uint64_t revision = 0;
    status = api->enumerate_devices(devices, 16, &count, &revision);
    if (status != INPUT_NEXT_STATUS_OK || count == 0) {
        return status == INPUT_NEXT_STATUS_OK ? INPUT_NEXT_STATUS_NO_DATA : status;
    }

    input_next_input_snapshot_v1 latest = {0};
    latest.struct_size = sizeof(latest);
    status = api->read_latest(&devices[0].target, &latest);
    if (status != INPUT_NEXT_STATUS_OK && status != INPUT_NEXT_STATUS_NO_DATA) {
        return status;
    }
    printf("Latest buttons: %u\n", latest.state.buttons);

    input_next_reader_request_v1 request = {0};
    request.struct_size = sizeof(request);
    request.focus_mode = INPUT_NEXT_CORE_READER_FOREGROUND_PROCESS;
    request.target = devices[0].target;
    input_next_reader_v1 reader = {0};
    reader.struct_size = sizeof(reader);
    status = api->open_reader(&request, &reader);
    if (status != INPUT_NEXT_STATUS_OK) {
        return status;
    }

    input_next_input_snapshot_v1 snapshots[32] = {0};
    for (uint32_t index = 0; index < 32; ++index) {
        snapshots[index].struct_size = sizeof(snapshots[index]);
    }
    input_next_reader_result_v1 result = {0};
    result.struct_size = sizeof(result);
    status = api->read_events(&reader, snapshots, 32, &result);
    printf("History records: %u, status: %u\n", result.count, status);
    const input_next_status close_status = api->close_reader(&reader);
    return close_status == INPUT_NEXT_STATUS_OK ? status : close_status;
}
