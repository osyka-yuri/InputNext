#pragma once

#include "dinput/native_types.hpp"

#include <cstddef>
#include <cstdint>

struct input_next_directinput_api_v1 final {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
    HRESULT(WINAPI* create)(HINSTANCE, DWORD, REFIID, void**, IUnknown*) noexcept;
    HRESULT(WINAPI* get_class_object)(REFCLSID, REFIID, void**) noexcept;
    HRESULT(WINAPI* can_unload)() noexcept;
    LPCDIDATAFORMAT(WINAPI* joystick_format)() noexcept;
};

using input_next_query_directinput_api_fn = const input_next_directinput_api_v1*(
    __cdecl*)(std::uint32_t) noexcept;

[[nodiscard]] inline bool is_valid_input_next_directinput_api_v1(
    const input_next_directinput_api_v1* api) noexcept {
    return api != nullptr && api->struct_size == sizeof(*api) && api->abi_version == 1U &&
           api->create != nullptr && api->get_class_object != nullptr &&
           api->can_unload != nullptr && api->joystick_format != nullptr;
}

extern "C" const input_next_directinput_api_v1* __cdecl input_next_runtime_query_directinput_api(
    std::uint32_t requested_abi) noexcept;

static_assert(offsetof(input_next_directinput_api_v1, create) == 8U);
static_assert(sizeof(input_next_directinput_api_v1) == 8U + (4U * sizeof(void*)));
