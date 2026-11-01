#include "external_callback_fixture_protocol.hpp"

#include "runtime/abi/xinput/api.hpp"

namespace {

struct external_get_state_request final {
    input_next_xinput_flavor flavor;
    DWORD user_index;
    XINPUT_STATE* state;
};

[[nodiscard]] DWORD execute_external_get_state(const external_get_state_request& request) noexcept {
    static_cast<void>(request);
    return ERROR_SUCCESS;
}

input_next_runtime_api_v1 copied_api{};
bool copied_api_initialized{};

} // namespace

extern "C" DWORD WINAPI inputnext_external_get_state(const input_next_xinput_flavor flavor,
    const DWORD user_index,
    XINPUT_STATE* const state) noexcept {
    return execute_external_get_state({.flavor = flavor, .user_index = user_index, .state = state});
}

extern "C" BOOL __cdecl inputnext_external_copy_api(
    const input_next_runtime_api_v1* const api) noexcept {
    if (copied_api_initialized || !is_valid_input_next_runtime_api_v1(api)) {
        return FALSE;
    }
    copied_api = *api;
    copied_api_initialized = true;
    return TRUE;
}

extern "C" const input_next_runtime_api_v1* __cdecl inputnext_external_get_api() noexcept {
    return copied_api_initialized ? &copied_api : nullptr;
}
