#pragma once

#include "native_types.hpp"

namespace inputnext::dinput {

[[nodiscard]] HRESULT create_system_input(void* context,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* outer) noexcept;
[[nodiscard]] LPCDIDATAFORMAT system_joystick_format() noexcept;

} // namespace inputnext::dinput
