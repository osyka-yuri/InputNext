#include <input_next_core.h>
#include <input_next_effects.h>

#include <stddef.h>
#include <stdint.h>

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(void*) == 4U, "supported 32-bit pointer width");
#elif UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(void*) == 8U, "supported 64-bit pointer width");
#else
#    error Unsupported pointer size
#endif
_Static_assert(sizeof(input_next_effects_trigger_v1) == 20U, "trigger ABI size");
_Static_assert(sizeof(input_next_effects_rumble_v1) == 8U, "rumble ABI size");
_Static_assert(sizeof(input_next_effects_led_v1) == 16U, "LED ABI size");
_Static_assert(sizeof(input_next_effects_context_v1) == 32U, "context ABI size");
_Static_assert(sizeof(input_next_effects_capabilities_v1) == 32U, "capabilities ABI size");
_Static_assert(sizeof(input_next_effects_open_v1) == 48U, "open ABI size");
_Static_assert(sizeof(input_next_effects_context_info_v1) == 40U, "context info ABI size");
_Static_assert(sizeof(input_next_effects_request_v1) == 120U, "submit ABI size");
_Static_assert(sizeof(input_next_effects_receipt_v1) == 88U, "receipt ABI size");
_Static_assert(sizeof(input_next_effects_clear_v1) == 80U, "clear ABI size");
_Static_assert(sizeof(input_next_effects_pcm_v1) == 32U, "PCM descriptor ABI size");
_Static_assert(sizeof(input_next_effects_body_request_v1) == 112U, "body submit ABI size");
_Static_assert(sizeof(input_next_effects_body_receipt_v1) == 120U, "body receipt ABI size");
_Static_assert(sizeof(input_next_effects_body_clear_v1) == 64U, "body clear ABI size");
_Static_assert(sizeof(input_next_effects_close_v1) == 48U, "close ABI size");
_Static_assert(sizeof(input_next_effects_close_receipt_v1) == 96U, "close receipt ABI size");
_Static_assert(sizeof(input_next_effects_progress_query_v1) == 56U, "progress query ABI size");
_Static_assert(sizeof(input_next_effects_progress_v1) == 80U, "progress ABI size");
_Static_assert(sizeof(input_next_effects_api_v1) == 8U + (10U * sizeof(void*)), "table ABI size");

_Static_assert(offsetof(input_next_device_target_v1, runtime_id) == 8U,
    "target runtime identity offset");
_Static_assert(offsetof(input_next_device_target_v1, device_key) == 24U,
    "target device key offset");
_Static_assert(offsetof(input_next_device_target_v1, connection_id) == 32U,
    "target connection id offset");
_Static_assert(offsetof(input_next_effects_context_v1, runtime_id) == 8U,
    "context runtime identity offset");
_Static_assert(offsetof(input_next_effects_context_v1, id) == 24U, "context id offset");
_Static_assert(offsetof(input_next_effects_request_v1, context) == 16U, "submit context offset");
_Static_assert(offsetof(input_next_effects_request_v1, mutation_sequence) == 48U,
    "submit sequence offset");
_Static_assert(offsetof(input_next_effects_receipt_v1, context) == 8U,
    "discrete receipt context offset");
_Static_assert(offsetof(input_next_effects_receipt_v1, channel_revisions) == 56U,
    "discrete receipt revisions offset");
_Static_assert(offsetof(input_next_effects_body_request_v1, context) == 16U,
    "body submit context offset");
_Static_assert(offsetof(input_next_effects_body_request_v1, lease_id) == 48U,
    "body submit lease offset");
_Static_assert(offsetof(input_next_effects_body_request_v1, pcm) == 80U,
    "body submit PCM descriptor offset");
_Static_assert(offsetof(input_next_effects_body_receipt_v1, target) == 40U,
    "body receipt target offset");
_Static_assert(offsetof(input_next_effects_body_receipt_v1, fallback_reason) == 112U,
    "body receipt fallback offset");
_Static_assert(offsetof(input_next_effects_close_receipt_v1, target) == 40U,
    "close receipt target offset");
_Static_assert(offsetof(input_next_effects_close_receipt_v1, publication_revision) == 88U,
    "close receipt publication offset");
_Static_assert(offsetof(input_next_effects_progress_v1, target) == 16U, "progress target offset");
_Static_assert(offsetof(input_next_effects_progress_v1, attempt_serial) == 72U,
    "progress attempt serial offset");
_Static_assert(offsetof(input_next_effects_pcm_v1, samples) == 24U, "PCM pointer offset");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(input_next_effects_pcm_v1, pointer_padding) == 28U,
    "x86 PCM pointer padding offset");
#endif

_Static_assert(offsetof(input_next_effects_api_v1, resolve_target) == 8U + (0U * sizeof(void*)),
    "resolve is the first table entry");
_Static_assert(offsetof(input_next_effects_api_v1, open_context) == 8U + (1U * sizeof(void*)),
    "open is the second table entry");
_Static_assert(offsetof(input_next_effects_api_v1, query_context) == 8U + (2U * sizeof(void*)),
    "context query is the third table entry");
_Static_assert(offsetof(input_next_effects_api_v1, submit) == 8U + (3U * sizeof(void*)),
    "discrete submit is the fourth table entry");
_Static_assert(offsetof(input_next_effects_api_v1, clear) == 8U + (4U * sizeof(void*)),
    "discrete clear is the fifth table entry");
_Static_assert(offsetof(input_next_effects_api_v1, submit_body) == 8U + (5U * sizeof(void*)),
    "body submit is the sixth table entry");
_Static_assert(offsetof(input_next_effects_api_v1, clear_body) == 8U + (6U * sizeof(void*)),
    "body clear is the seventh table entry");
_Static_assert(offsetof(input_next_effects_api_v1, close_context) == 8U + (7U * sizeof(void*)),
    "close is the eighth table entry");
_Static_assert(offsetof(input_next_effects_api_v1, ack_close) == 8U + (8U * sizeof(void*)),
    "ACK is the ninth table entry");
_Static_assert(offsetof(input_next_effects_api_v1, query_progress) == 8U + (9U * sizeof(void*)),
    "progress is the tenth table entry");

typedef const input_next_effects_api_v1*(INPUT_NEXT_EFFECTS_CALL* effects_query_fn)(uint32_t);

effects_query_fn inputnext_effects_header_c_query_typecheck(void) {
    return &input_next_runtime_query_effects_api;
}
