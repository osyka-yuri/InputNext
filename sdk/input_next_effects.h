#ifndef INPUT_NEXT_EFFECTS_H
#define INPUT_NEXT_EFFECTS_H

#include <stdint.h>

#include "input_next_core.h"

#define INPUT_NEXT_EFFECTS_ABI_VERSION UINT32_C(1)
#define INPUT_NEXT_EFFECTS_MAX_TTL_MS UINT32_C(60000)

#define INPUT_NEXT_EFFECTS_LEFT_TRIGGER UINT32_C(0x01)
#define INPUT_NEXT_EFFECTS_RIGHT_TRIGGER UINT32_C(0x02)
#define INPUT_NEXT_EFFECTS_RUMBLE UINT32_C(0x04)
#define INPUT_NEXT_EFFECTS_LEDS UINT32_C(0x08)
#define INPUT_NEXT_EFFECTS_ALL_CHANNELS UINT32_C(0x0f)

#define INPUT_NEXT_EFFECTS_CAP_LEFT_TRIGGER UINT32_C(0x01)
#define INPUT_NEXT_EFFECTS_CAP_RIGHT_TRIGGER UINT32_C(0x02)
#define INPUT_NEXT_EFFECTS_CAP_LOW_RUMBLE UINT32_C(0x04)
#define INPUT_NEXT_EFFECTS_CAP_HIGH_RUMBLE UINT32_C(0x08)
#define INPUT_NEXT_EFFECTS_CAP_LIGHTBAR UINT32_C(0x10)
#define INPUT_NEXT_EFFECTS_CAP_PLAYER_LEDS UINT32_C(0x20)
#define INPUT_NEXT_EFFECTS_CAP_MIC_LED UINT32_C(0x40)
#define INPUT_NEXT_EFFECTS_CAP_BODY_LEASE UINT32_C(0x80)
#define INPUT_NEXT_EFFECTS_CAP_HD_READY UINT32_C(0x100)

#define INPUT_NEXT_EFFECTS_COMPLETION_CAPABILITY_SETTER_RETURNED_CURRENT UINT32_C(0x01)
#define INPUT_NEXT_EFFECTS_COMPLETION_CAPABILITY_RAW_OUTPUT_ACCEPTED_CURRENT UINT32_C(0x02)

#define INPUT_NEXT_EFFECTS_TRIGGER_OFF UINT32_C(0)
#define INPUT_NEXT_EFFECTS_TRIGGER_FEEDBACK UINT32_C(1)
#define INPUT_NEXT_EFFECTS_TRIGGER_WEAPON UINT32_C(2)
#define INPUT_NEXT_EFFECTS_TRIGGER_VIBRATION UINT32_C(3)

#define INPUT_NEXT_EFFECTS_CONTEXT_ATTACHING UINT32_C(1)
#define INPUT_NEXT_EFFECTS_CONTEXT_OPEN UINT32_C(2)
#define INPUT_NEXT_EFFECTS_CONTEXT_REVOKED UINT32_C(3)

#define INPUT_NEXT_EFFECTS_BODY_ROUTE_CLASSIC UINT32_C(0)
#define INPUT_NEXT_EFFECTS_BODY_ROUTE_PREFER_HD UINT32_C(1)

#define INPUT_NEXT_EFFECTS_BODY_PCM_KEEP UINT32_C(0)
#define INPUT_NEXT_EFFECTS_BODY_PCM_REPLACE UINT32_C(1)
#define INPUT_NEXT_EFFECTS_BODY_PCM_STOP UINT32_C(2)

#define INPUT_NEXT_EFFECTS_BODY_EFFECTIVE_CLASSIC UINT32_C(0)
#define INPUT_NEXT_EFFECTS_BODY_EFFECTIVE_HD UINT32_C(1)

#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_NONE UINT32_C(0)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_NOT_READY UINT32_C(1)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_UNSUPPORTED_TRANSPORT UINT32_C(2)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_INSTANCE_LOST UINT32_C(3)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_OUTPUT_FAILED UINT32_C(4)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_PLAY_FAILED UINT32_C(5)
#define INPUT_NEXT_EFFECTS_BODY_FALLBACK_PREEMPTED UINT32_C(6)

#define INPUT_NEXT_EFFECTS_CLOSE_NORMAL UINT32_C(1)
#define INPUT_NEXT_EFFECTS_CLOSE_CONNECTION_RETIRED UINT32_C(2)
#define INPUT_NEXT_EFFECTS_CLOSE_CANCELLED_BEFORE_CREATION UINT32_C(3)

#define INPUT_NEXT_EFFECTS_PROGRESS_DISPATCH_PENDING UINT32_C(0)
#define INPUT_NEXT_EFFECTS_PROGRESS_CURRENT_COMPLETED UINT32_C(1)
#define INPUT_NEXT_EFFECTS_PROGRESS_SUPERSEDED UINT32_C(2)
#define INPUT_NEXT_EFFECTS_PROGRESS_CONNECTION_RETIRED UINT32_C(3)
#define INPUT_NEXT_EFFECTS_PROGRESS_UNKNOWN UINT32_C(4)
#define INPUT_NEXT_EFFECTS_PROGRESS_NOT_DELIVERABLE UINT32_C(5)

#define INPUT_NEXT_EFFECTS_COMPLETION_NONE UINT32_C(0)
#define INPUT_NEXT_EFFECTS_COMPLETION_SETTER_RETURNED_CURRENT UINT32_C(1)
#define INPUT_NEXT_EFFECTS_COMPLETION_RAW_OUTPUT_ACCEPTED_CURRENT UINT32_C(2)

#ifdef _WIN32
#    define INPUT_NEXT_EFFECTS_CALL __cdecl
#else
#    define INPUT_NEXT_EFFECTS_CALL
#endif

#ifdef __cplusplus
#    define INPUT_NEXT_EFFECTS_NOEXCEPT noexcept
extern "C" {
#else
#    define INPUT_NEXT_EFFECTS_NOEXCEPT
#endif

#pragma pack(push, 8)

typedef struct input_next_effects_trigger_v1 {
    uint32_t mode;
    float start;
    float end;
    float strength;
    float frequency; /* Device parameter in [0, 255], without a physical-Hz promise. */
} input_next_effects_trigger_v1;

typedef struct input_next_effects_rumble_v1 {
    float low_frequency;
    float high_frequency;
} input_next_effects_rumble_v1;

typedef struct input_next_effects_led_v1 {
    uint32_t lightbar_rgb;   /* 0x00RRGGBB */
    uint32_t player_index;   /* 0=off, 1..4 existing physical patterns */
    uint32_t microphone_led; /* 0 or 1 */
    uint32_t reserved;
} input_next_effects_led_v1;

typedef struct input_next_effects_capabilities_v1 {
    uint32_t struct_size;
    uint32_t supported_channels;
    uint32_t feature_flags;
    uint32_t completion_capabilities;
    uint64_t audio_instance_id;
    uint64_t capabilities_revision;
} input_next_effects_capabilities_v1;

typedef struct input_next_effects_open_v1 {
    uint32_t struct_size;
    uint32_t flags;
    input_next_device_target_v1 target;
} input_next_effects_open_v1;

typedef struct input_next_effects_context_info_v1 {
    uint32_t struct_size;
    uint32_t state;
    input_next_effects_context_v1 context;
} input_next_effects_context_info_v1;

typedef struct input_next_effects_request_v1 {
    uint32_t struct_size;
    uint32_t channel_mask;
    uint32_t ttl_ms;
    uint32_t reserved;
    input_next_effects_context_v1 context;
    uint64_t mutation_sequence;
    input_next_effects_trigger_v1 left_trigger;
    input_next_effects_trigger_v1 right_trigger;
    input_next_effects_rumble_v1 rumble;
    input_next_effects_led_v1 leds;
} input_next_effects_request_v1;

typedef struct input_next_effects_receipt_v1 {
    uint32_t struct_size;
    uint32_t accepted_mask;
    input_next_effects_context_v1 context;
    uint64_t mutation_sequence;
    uint64_t publication_revision;
    uint64_t channel_revisions[4];
} input_next_effects_receipt_v1;

typedef struct input_next_effects_clear_v1 {
    uint32_t struct_size;
    uint32_t channel_mask;
    input_next_effects_context_v1 context;
    uint64_t mutation_sequence;
    uint64_t channel_revisions[4];
} input_next_effects_clear_v1;

typedef struct input_next_effects_pcm_v1 {
    uint32_t struct_size;
    uint32_t frame_count;
    uint32_t channels;
    uint32_t sample_rate;
    float gain;
    uint32_t reserved;
    const float* samples;
#if UINTPTR_MAX == UINT32_MAX
    uint32_t pointer_padding;
#elif UINTPTR_MAX != UINT64_MAX
#    error Unsupported pointer size
#endif
} input_next_effects_pcm_v1;

typedef struct input_next_effects_body_request_v1 {
    uint32_t struct_size;
    uint32_t ttl_ms;
    uint32_t route_preference;
    uint32_t pcm_command;
    input_next_effects_context_v1 context;
    uint64_t lease_id;
    uint64_t audio_instance_id;
    uint64_t sequence;
    input_next_effects_rumble_v1 fallback_rumble;
    input_next_effects_pcm_v1 pcm;
} input_next_effects_body_request_v1;

typedef struct input_next_effects_body_receipt_v1 {
    uint32_t struct_size;
    uint32_t effective_route;
    input_next_effects_context_v1 context;
    input_next_device_target_v1 target;
    uint64_t lease_id;
    uint64_t audio_instance_id;
    uint64_t sequence;
    uint64_t publication_revision;
    uint32_t fallback_reason;
    uint32_t reserved;
} input_next_effects_body_receipt_v1;

typedef struct input_next_effects_body_clear_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    input_next_effects_context_v1 context;
    uint64_t lease_id;
    uint64_t audio_instance_id;
    uint64_t sequence;
} input_next_effects_body_clear_v1;

typedef struct input_next_effects_close_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    input_next_effects_context_v1 context;
    uint64_t close_request_id;
} input_next_effects_close_v1;

typedef struct input_next_effects_close_receipt_v1 {
    uint32_t struct_size;
    uint32_t disposition;
    input_next_effects_context_v1 context;
    input_next_device_target_v1 target;
    uint64_t close_request_id;
    uint64_t publication_revision;
} input_next_effects_close_receipt_v1;

typedef struct input_next_effects_progress_query_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    input_next_device_target_v1 target;
    uint64_t publication_revision;
} input_next_effects_progress_query_v1;

typedef struct input_next_effects_progress_v1 {
    uint32_t struct_size;
    uint32_t stage;
    uint32_t completion_class;
    input_next_status completion_status;
    input_next_device_target_v1 target;
    uint64_t requested_publication_revision;
    uint64_t current_publication_revision;
    uint64_t attempt_serial;
} input_next_effects_progress_v1;

typedef struct input_next_effects_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* resolve_target)(const input_next_device_selector_v1* selector,
        input_next_device_target_v1* out_target,
        input_next_effects_capabilities_v1* out_capabilities) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* open_context)(const input_next_effects_open_v1* request,
        input_next_effects_context_v1* out_context) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* query_context)(const input_next_effects_context_v1* context,
        input_next_effects_context_info_v1* out_info) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(INPUT_NEXT_EFFECTS_CALL* submit)(const input_next_effects_request_v1* request,
        input_next_effects_receipt_v1* out_receipt) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(INPUT_NEXT_EFFECTS_CALL* clear)(
        const input_next_effects_clear_v1* request) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* submit_body)(const input_next_effects_body_request_v1* request,
        input_next_effects_body_receipt_v1* out_receipt) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(INPUT_NEXT_EFFECTS_CALL* clear_body)(
        const input_next_effects_body_clear_v1* request) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* close_context)(const input_next_effects_close_v1* request,
        input_next_effects_close_receipt_v1* out_receipt) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(INPUT_NEXT_EFFECTS_CALL* ack_close)(
        const input_next_effects_close_receipt_v1* receipt) INPUT_NEXT_EFFECTS_NOEXCEPT;
    input_next_status(
        INPUT_NEXT_EFFECTS_CALL* query_progress)(const input_next_effects_progress_query_v1* query,
        input_next_effects_progress_v1* out_progress) INPUT_NEXT_EFFECTS_NOEXCEPT;
} input_next_effects_api_v1;

#pragma pack(pop)

const input_next_effects_api_v1* INPUT_NEXT_EFFECTS_CALL input_next_runtime_query_effects_api(
    uint32_t requested_abi) INPUT_NEXT_EFFECTS_NOEXCEPT;

typedef const input_next_effects_api_v1*(
    INPUT_NEXT_EFFECTS_CALL* pfn_input_next_runtime_query_effects_api)(
    uint32_t requested_abi)INPUT_NEXT_EFFECTS_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* INPUT_NEXT_EFFECTS_H */
