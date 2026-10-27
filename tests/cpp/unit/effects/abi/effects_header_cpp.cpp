#include <input_next_core.h>
#include <input_next_effects.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

using effects_query_fn = const input_next_effects_api_v1*(
    INPUT_NEXT_EFFECTS_CALL*)(std::uint32_t) noexcept;
using effects_resolve_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_device_selector_v1*,
    input_next_device_target_v1*,
    input_next_effects_capabilities_v1*) noexcept;
using effects_open_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_open_v1*,
    input_next_effects_context_v1*) noexcept;
using effects_query_context_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_context_v1*,
    input_next_effects_context_info_v1*) noexcept;
using effects_submit_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_request_v1*,
    input_next_effects_receipt_v1*) noexcept;
using effects_clear_fn = input_next_status(INPUT_NEXT_EFFECTS_CALL*)(
    const input_next_effects_clear_v1*) noexcept;
using effects_submit_body_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_body_request_v1*,
    input_next_effects_body_receipt_v1*) noexcept;
using effects_clear_body_fn = input_next_status(INPUT_NEXT_EFFECTS_CALL*)(
    const input_next_effects_body_clear_v1*) noexcept;
using effects_close_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_close_v1*,
    input_next_effects_close_receipt_v1*) noexcept;
using effects_ack_close_fn = input_next_status(INPUT_NEXT_EFFECTS_CALL*)(
    const input_next_effects_close_receipt_v1*) noexcept;
using effects_query_progress_fn = input_next_status(
    INPUT_NEXT_EFFECTS_CALL*)(const input_next_effects_progress_query_v1*,
    input_next_effects_progress_v1*) noexcept;

static_assert(std::is_same_v<decltype(&input_next_runtime_query_effects_api), effects_query_fn>);
static_assert(
    std::is_same_v<decltype(input_next_effects_api_v1::resolve_target), effects_resolve_fn>);
static_assert(std::is_same_v<decltype(input_next_effects_api_v1::open_context), effects_open_fn>);
static_assert(
    std::is_same_v<decltype(input_next_effects_api_v1::query_context), effects_query_context_fn>);
static_assert(std::is_same_v<decltype(input_next_effects_api_v1::submit), effects_submit_fn>);
static_assert(std::is_same_v<decltype(input_next_effects_api_v1::clear), effects_clear_fn>);
static_assert(
    std::is_same_v<decltype(input_next_effects_api_v1::submit_body), effects_submit_body_fn>);
static_assert(
    std::is_same_v<decltype(input_next_effects_api_v1::clear_body), effects_clear_body_fn>);
static_assert(std::is_same_v<decltype(input_next_effects_api_v1::close_context), effects_close_fn>);
static_assert(std::is_same_v<decltype(input_next_effects_api_v1::ack_close), effects_ack_close_fn>);
static_assert(
    std::is_same_v<decltype(input_next_effects_api_v1::query_progress), effects_query_progress_fn>);
static_assert(sizeof(input_next_effects_pcm_v1) == 32U);
static_assert(sizeof(input_next_effects_body_request_v1) == 112U);
static_assert(sizeof(input_next_effects_body_receipt_v1) == 120U);
static_assert(sizeof(input_next_effects_api_v1) == (8U + (10U * sizeof(void*))));
static_assert(offsetof(input_next_effects_api_v1, resolve_target) == 8U);
static_assert(offsetof(input_next_effects_api_v1, open_context) == 8U + (1U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, query_context) == 8U + (2U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, submit) == 8U + (3U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, clear) == 8U + (4U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, submit_body) == 8U + (5U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, clear_body) == 8U + (6U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, close_context) == 8U + (7U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, ack_close) == 8U + (8U * sizeof(void*)));
static_assert(offsetof(input_next_effects_api_v1, query_progress) == 8U + (9U * sizeof(void*)));
