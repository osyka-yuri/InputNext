#pragma once

#include "dinput/native_types.hpp"

namespace inputnext::dinput {
struct host;

namespace detail {

// The caller validates the callback and retains its root and system COM references
// until enumeration and every application callback have returned.
[[nodiscard]] HRESULT enumerate_devices(const host& root_host,
    HINSTANCE application,
    IDirectInput8W& system,
    DWORD device_type,
    LPDIENUMDEVICESCALLBACKA callback,
    LPVOID context,
    DWORD flags) noexcept;

[[nodiscard]] HRESULT enumerate_devices(const host& root_host,
    HINSTANCE application,
    IDirectInput8W& system,
    DWORD device_type,
    LPDIENUMDEVICESCALLBACKW callback,
    LPVOID context,
    DWORD flags) noexcept;

} // namespace detail
} // namespace inputnext::dinput
