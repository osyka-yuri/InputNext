#pragma once

#include <stddef.h>
#include <stdint.h>

#pragma pack(push, 1)
#include <input_next_core.h> // IWYU pragma: export

struct inputnext_packing_probe {
    uint8_t prefix;
    uint64_t suffix;
};

#pragma pack(pop)

#ifdef __cplusplus
#    define INPUTNEXT_ABI_ASSERT(condition) static_assert(condition)
#else
#    define INPUTNEXT_ABI_ASSERT(condition) _Static_assert(condition, #condition)
#endif

INPUTNEXT_ABI_ASSERT(sizeof(struct inputnext_packing_probe) == 9);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_device_target_v1) == 40);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_input_domain_v1) == 48);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_handle_v1) == 32);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_reader_v1) == 32);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_effects_context_v1) == 32);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_runtime_info_v1) == 48);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_device_selector_v1) == 96);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_device_info_v1) == 232);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_gamepad_state_v1) == 28);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_input_snapshot_v1) == 112);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_reader_request_v1) == 56);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_reader_result_v1) == 112);
INPUTNEXT_ABI_ASSERT(sizeof(input_next_core_api_v1) == 8 + (7 * sizeof(void*)));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_target_v1, runtime_id) == 8);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_target_v1, connection_id) == 32);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_selector_v1, target) == 16);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_selector_v1, hardware_fingerprint) == 56);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_info_v1, hardware_fingerprint) == 72);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_device_info_v1, display_name) == 104);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_input_snapshot_v1, sequence) == 56);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_input_snapshot_v1, state) == 80);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_reader_result_v1, first_sequence) == 64);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_reader_result_v1, lost_count) == 96);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, get_runtime_info) == 8);
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, enumerate_devices) == 8 + sizeof(void*));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, resolve_target) == 8 + (2 * sizeof(void*)));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, read_latest) == 8 + (3 * sizeof(void*)));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, open_reader) == 8 + (4 * sizeof(void*)));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, read_events) == 8 + (5 * sizeof(void*)));
INPUTNEXT_ABI_ASSERT(offsetof(input_next_core_api_v1, close_reader) == 8 + (6 * sizeof(void*)));

#undef INPUTNEXT_ABI_ASSERT
