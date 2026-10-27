#include "effects_api.hpp"

#include "runtime/abi/runtime_api_internal.hpp"
#include "runtime/bootstrap/query_front.hpp"

#include <input_next_effects.h>

#include <cstdint>

namespace inputnext::runtime_detail {
namespace {

constexpr input_next_effects_api_v1 api_v1{.struct_size = sizeof(input_next_effects_api_v1),
    .abi_version = INPUT_NEXT_EFFECTS_ABI_VERSION,
    .resolve_target = &api_resolve_target,
    .open_context = &api_open_context,
    .query_context = &api_query_context,
    .submit = &api_submit,
    .clear = &api_clear,
    .submit_body = &api_submit_body,
    .clear_body = &api_clear_body,
    .close_context = &api_close_context,
    .ack_close = &api_ack_close,
    .query_progress = &api_query_progress};

static_assert(api_v1.struct_size == sizeof(input_next_effects_api_v1));
static_assert(api_v1.abi_version == INPUT_NEXT_EFFECTS_ABI_VERSION);
static_assert(api_v1.resolve_target != nullptr && api_v1.open_context != nullptr &&
              api_v1.query_context != nullptr && api_v1.submit != nullptr &&
              api_v1.clear != nullptr && api_v1.submit_body != nullptr &&
              api_v1.clear_body != nullptr && api_v1.close_context != nullptr &&
              api_v1.ack_close != nullptr && api_v1.query_progress != nullptr);

} // namespace

const input_next_effects_api_v1* effects_api_v1() noexcept {
    return &api_v1;
}

} // namespace inputnext::runtime_detail

extern "C" const input_next_effects_api_v1* INPUT_NEXT_EFFECTS_CALL
input_next_runtime_query_effects_api(const std::uint32_t requested_abi) noexcept {
    if (requested_abi != INPUT_NEXT_EFFECTS_ABI_VERSION) {
        return nullptr;
    }
    const auto* const private_api = inputnext::runtime_detail::query_runtime_api(
        inputnext::runtime_detail::runtime_query_front(),
        1U);
    return private_api != nullptr ? inputnext::runtime_detail::effects_api_v1() : nullptr;
}
