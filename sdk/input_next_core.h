#ifndef INPUT_NEXT_CORE_H
#define INPUT_NEXT_CORE_H

#include <stdint.h>

#define INPUT_NEXT_CORE_ABI_VERSION UINT32_C(1)

#define INPUT_NEXT_STATUS_OK UINT32_C(0)
#define INPUT_NEXT_STATUS_INVALID_ARGUMENT UINT32_C(1)
#define INPUT_NEXT_STATUS_UNAVAILABLE UINT32_C(2)
#define INPUT_NEXT_STATUS_STALE_TARGET UINT32_C(3)
#define INPUT_NEXT_STATUS_UNSUPPORTED_TARGET UINT32_C(4)
#define INPUT_NEXT_STATUS_UNSUPPORTED_CHANNEL UINT32_C(5)
#define INPUT_NEXT_STATUS_SCHEDULER_UNAVAILABLE UINT32_C(6)
#define INPUT_NEXT_STATUS_BUSY UINT32_C(7)
#define INPUT_NEXT_STATUS_STALE_LEASE UINT32_C(8)
#define INPUT_NEXT_STATUS_BUFFER_TOO_SMALL UINT32_C(9)
#define INPUT_NEXT_STATUS_CAPACITY_EXCEEDED UINT32_C(10)
#define INPUT_NEXT_STATUS_STALE_CONTEXT UINT32_C(11)
#define INPUT_NEXT_STATUS_STALE_READER UINT32_C(12)
/* Values 13 and 19 are reserved and are rejected as unknown. */
#define INPUT_NEXT_STATUS_SOURCE_RETIRED UINT32_C(14)
#define INPUT_NEXT_STATUS_STALE_DOMAIN UINT32_C(15)
#define INPUT_NEXT_STATUS_STALE_EFFECT UINT32_C(16)
#define INPUT_NEXT_STATUS_STALE_SEQUENCE UINT32_C(17)
#define INPUT_NEXT_STATUS_TIMED_OUT_UNKNOWN UINT32_C(18)
#define INPUT_NEXT_STATUS_UNSUPPORTED UINT32_C(20)
#define INPUT_NEXT_STATUS_PREPARATION_FAILED UINT32_C(21)
#define INPUT_NEXT_STATUS_IO_FAILED UINT32_C(22)
#define INPUT_NEXT_STATUS_RETRY_LIMIT UINT32_C(23)
#define INPUT_NEXT_STATUS_REVOKED UINT32_C(24)
#define INPUT_NEXT_STATUS_PENDING UINT32_C(25)
#define INPUT_NEXT_STATUS_NO_DATA UINT32_C(26)
#define INPUT_NEXT_STATUS_AMBIGUOUS_TARGET UINT32_C(27)
#define INPUT_NEXT_STATUS_NOT_ADMITTED UINT32_C(28)

#define INPUT_NEXT_CORE_SELECTOR_XINPUT_SLOT UINT32_C(1)
#define INPUT_NEXT_CORE_SELECTOR_EXACT_TARGET UINT32_C(2)
#define INPUT_NEXT_CORE_SELECTOR_HARDWARE_FINGERPRINT UINT32_C(3)

#define INPUT_NEXT_CORE_INPUT_PROFILE_GAMEPAD UINT32_C(1)

#define INPUT_NEXT_CORE_TRANSPORT_UNKNOWN UINT16_C(0)
#define INPUT_NEXT_CORE_TRANSPORT_USB UINT16_C(1)
#define INPUT_NEXT_CORE_TRANSPORT_BLUETOOTH UINT16_C(2)
#define INPUT_NEXT_CORE_TRANSPORT_OTHER_WIRELESS UINT16_C(3)

#define INPUT_NEXT_CORE_IDENTITY_NONE UINT32_C(0)
#define INPUT_NEXT_CORE_IDENTITY_PLATFORM UINT32_C(1)
#define INPUT_NEXT_CORE_IDENTITY_AMBIGUOUS UINT32_C(2)

#define INPUT_NEXT_CORE_SNAPSHOT_BASELINE UINT32_C(1)
#define INPUT_NEXT_CORE_READER_FOREGROUND_PROCESS UINT32_C(1)
#define INPUT_NEXT_CORE_READER_BACKGROUND UINT32_C(2)

#define INPUT_NEXT_CORE_READER_RESULT_CAUGHT_UP UINT32_C(1)
#define INPUT_NEXT_CORE_READER_RESULT_BASELINE UINT32_C(2)
#define INPUT_NEXT_CORE_READER_RESULT_GAP UINT32_C(4)

#define INPUT_NEXT_CORE_READER_LOSS_NONE UINT32_C(0)
#define INPUT_NEXT_CORE_READER_LOSS_KNOWN UINT32_C(1)
#define INPUT_NEXT_CORE_READER_LOSS_UNKNOWN UINT32_C(2)

#define INPUT_NEXT_CORE_READER_RESET_NONE UINT32_C(0)
#define INPUT_NEXT_CORE_READER_RESET_INITIAL_OPEN UINT32_C(1)
#define INPUT_NEXT_CORE_READER_RESET_HISTORY_GAP UINT32_C(2)

#ifdef _WIN32
#    define INPUT_NEXT_CORE_CALL __cdecl
#else
#    define INPUT_NEXT_CORE_CALL
#endif

#ifdef __cplusplus
#    define INPUT_NEXT_CORE_NOEXCEPT noexcept
extern "C" {
#else
#    define INPUT_NEXT_CORE_NOEXCEPT
#endif

typedef uint32_t input_next_status;

#pragma pack(push, 8)

typedef struct input_next_device_target_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t runtime_id[2];
    uint64_t device_key;
    uint64_t connection_id;
} input_next_device_target_v1;

typedef struct input_next_input_domain_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t runtime_id[2];
    uint64_t device_key;
    uint64_t connection_id;
    uint64_t stream_id;
} input_next_input_domain_v1;

typedef struct input_next_handle_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t runtime_id[2];
    uint64_t id;
} input_next_handle_v1;

typedef struct input_next_reader_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t runtime_id[2];
    uint64_t id;
} input_next_reader_v1;

typedef struct input_next_effects_context_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t runtime_id[2];
    uint64_t id;
} input_next_effects_context_v1;

typedef struct input_next_runtime_info_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t runtime_id[2];
    uint64_t qpc_frequency;
    uint32_t max_devices;
    uint32_t max_readers;
    uint32_t max_effect_contexts;
    uint32_t flags;
} input_next_runtime_info_v1;

typedef struct input_next_device_selector_v1 {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t xinput_slot;
    uint32_t reserved;
    input_next_device_target_v1 target;
    uint8_t hardware_fingerprint[32];
    uint32_t identity_flags;
    uint32_t reserved2;
} input_next_device_selector_v1;

typedef struct input_next_device_info_v1 {
    uint32_t struct_size;
    uint32_t flags;
    input_next_device_target_v1 target;
    uint32_t input_profile;
    uint32_t output_channels;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t revision;
    uint16_t transport;
    uint32_t identity_quality;
    uint32_t reserved;
    uint8_t hardware_fingerprint[32];
    uint16_t display_name[64];
} input_next_device_info_v1;

typedef struct input_next_gamepad_state_v1 {
    uint32_t buttons;
    float left_x;
    float left_y;
    float right_x;
    float right_y;
    float left_trigger;
    float right_trigger;
} input_next_gamepad_state_v1;

typedef struct input_next_input_snapshot_v1 {
    uint32_t struct_size;
    uint32_t flags;
    input_next_input_domain_v1 domain;
    uint64_t sequence;
    uint64_t source_timestamp_us;
    uint64_t qpc;
    input_next_gamepad_state_v1 state;
    uint32_t reserved;
} input_next_input_snapshot_v1;

typedef struct input_next_reader_request_v1 {
    uint32_t struct_size;
    uint32_t focus_mode;
    input_next_device_target_v1 target;
    uint32_t flags;
    uint32_t reserved;
} input_next_reader_request_v1;

typedef struct input_next_reader_result_v1 {
    uint32_t struct_size;
    uint32_t count;
    uint32_t flags;
    uint32_t loss_kind;
    input_next_input_domain_v1 domain;
    uint64_t first_sequence;
    uint64_t last_sequence;
    uint64_t checkpoint_sequence;
    uint64_t checkpoint_qpc;
    uint64_t lost_count;
    uint32_t reset_reason;
    uint32_t reserved;
} input_next_reader_result_v1;

typedef struct input_next_core_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    input_next_status(INPUT_NEXT_CORE_CALL* get_runtime_info)(
        input_next_runtime_info_v1* out_info) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_CORE_CALL* enumerate_devices)(input_next_device_info_v1* out_devices,
        uint32_t capacity,
        uint32_t* out_count,
        uint64_t* out_discovery_revision) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_CORE_CALL* resolve_target)(const input_next_device_selector_v1* selector,
        input_next_device_target_v1* out_target) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(INPUT_NEXT_CORE_CALL* read_latest)(const input_next_device_target_v1* target,
        input_next_input_snapshot_v1* out_snapshot) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_CORE_CALL* open_reader)(const input_next_reader_request_v1* request,
        input_next_reader_v1* out_reader) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(INPUT_NEXT_CORE_CALL* read_events)(const input_next_reader_v1* reader,
        input_next_input_snapshot_v1* out_snapshots,
        uint32_t capacity,
        input_next_reader_result_v1* out_result) INPUT_NEXT_CORE_NOEXCEPT;
    input_next_status(INPUT_NEXT_CORE_CALL* close_reader)(
        const input_next_reader_v1* reader) INPUT_NEXT_CORE_NOEXCEPT;
} input_next_core_api_v1;

#pragma pack(pop)

const input_next_core_api_v1* INPUT_NEXT_CORE_CALL input_next_runtime_query_core_api(
    uint32_t requested_abi) INPUT_NEXT_CORE_NOEXCEPT;

typedef const input_next_core_api_v1*(INPUT_NEXT_CORE_CALL* pfn_input_next_runtime_query_core_api)(
    uint32_t requested_abi)INPUT_NEXT_CORE_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* INPUT_NEXT_CORE_H */
