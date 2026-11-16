#pragma once

#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "lease.hpp"
#include "ownership.hpp"

#include <atomic>
#include <chrono>
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

namespace inputnext::dinput {

// Runs on the slot's worker. The request and receipt outlive run().
struct exclusive_input::actor final {
    input_access& access;
    const std::atomic<bool>& admission_closed;
    const exclusive_request& request;
    exclusive_lease::state& receipt;

    void run() const noexcept;

private:
    [[nodiscard]] bool exact_target_eligible() const noexcept;
    [[nodiscard]] bool can_continue() const noexcept;
    [[nodiscard]] HRESULT
    prepare(IDirectInput8W*& system, IDirectInputDevice8W*& device, bool& acquire_attempted) const;
    [[nodiscard]] HRESULT configure_device(IDirectInputDevice8W& device) const noexcept;
    void hold_lease(IDirectInputDevice8W* device, HRESULT preparation) const noexcept;
    void hold_background_lease(IDirectInputDevice8W& device) const noexcept;
    [[nodiscard]] HRESULT probe_authority(IDirectInputDevice8W& device,
        bool initial,
        bool requires_poll,
        std::chrono::steady_clock::time_point& next_probe) const noexcept;
    void monitor_authority(IDirectInputDevice8W& device,
        bool requires_poll,
        std::chrono::steady_clock::time_point next_probe) const noexcept;
};

namespace detail {

[[nodiscard]] bool valid_exclusive_path(const DIPROPGUIDANDPATH& path) noexcept;
[[nodiscard]] bool same_exclusive_path(const DIPROPGUIDANDPATH& left,
    const DIPROPGUIDANDPATH& right) noexcept;
[[nodiscard]] bool exclusive_target_eligible(const input_access::call& call,
    connection_target target,
    const focus::window_epoch& window) noexcept;

} // namespace detail
} // namespace inputnext::dinput
