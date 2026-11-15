#include "runtime_resolver.hpp"

#include <Windows.h>
#include <dinput.h>

extern "C" {

HRESULT WINAPI InputNextDirectInput8Create(HINSTANCE instance,
    DWORD version,
    REFIID iid,
    LPVOID* output,
    LPUNKNOWN outer) noexcept {
    if (output == nullptr) {
        return E_POINTER;
    }
    *output = nullptr;
    const auto* const api = inputnext::dinput_facade_detail::runtime_api();
    return api != nullptr ? api->create(instance, version, iid, output, outer) : E_FAIL;
}

HRESULT WINAPI InputNextDllCanUnloadNow() noexcept {
    const auto* const api = inputnext::dinput_facade_detail::runtime_api();
    return api != nullptr ? api->can_unload() : S_FALSE;
}

HRESULT WINAPI InputNextDllGetClassObject(REFCLSID class_id, REFIID iid, LPVOID* output) noexcept {
    if (output == nullptr) {
        return E_POINTER;
    }
    *output = nullptr;
    const auto* const api = inputnext::dinput_facade_detail::runtime_api();
    return api != nullptr ? api->get_class_object(class_id, iid, output) : E_FAIL;
}

HRESULT WINAPI InputNextDllRegisterServer() noexcept {
    return E_NOTIMPL;
}

HRESULT WINAPI InputNextDllUnregisterServer() noexcept {
    return E_NOTIMPL;
}

LPCDIDATAFORMAT WINAPI InputNextGetdfDIJoystick() noexcept {
    const auto* const api = inputnext::dinput_facade_detail::runtime_api();
    return api != nullptr ? api->joystick_format() : nullptr;
}

} // extern "C"
