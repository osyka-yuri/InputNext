#pragma once

#include "runtime/abi/xinput/api.hpp"

#ifdef INPUTNEXT_EXTERNAL_CALLBACK_FIXTURE_IMPORT
#    define INPUTNEXT_EXTERNAL_CALLBACK_IMPORT __declspec(dllimport)
#else
#    define INPUTNEXT_EXTERNAL_CALLBACK_IMPORT
#endif

extern "C" {

INPUTNEXT_EXTERNAL_CALLBACK_IMPORT DWORD WINAPI inputnext_external_get_state(
    input_next_xinput_flavor flavor,
    DWORD user_index,
    XINPUT_STATE* state) noexcept;

INPUTNEXT_EXTERNAL_CALLBACK_IMPORT BOOL __cdecl inputnext_external_copy_api(
    const input_next_runtime_api_v1* api) noexcept;
INPUTNEXT_EXTERNAL_CALLBACK_IMPORT const input_next_runtime_api_v1* __cdecl
inputnext_external_get_api() noexcept;

} // extern "C"

#undef INPUTNEXT_EXTERNAL_CALLBACK_IMPORT
