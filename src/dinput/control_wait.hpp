#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

namespace inputnext::dinput {

// A nested native wait can block the sender of the message being dispatched.
[[nodiscard]] bool native_control_reentrant() noexcept;

// Wait for a handle while dispatching sent messages. Earlier handles have priority.
[[nodiscard]] HRESULT wait_for_native_control(std::span<const HANDLE> events,
    std::size_t& signalled,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) noexcept;

} // namespace inputnext::dinput
