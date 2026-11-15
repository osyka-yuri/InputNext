#pragma once

#include "dinput/device_info.hpp"
#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"

#include <vector>

namespace inputnext::dinput {

struct catalogue_projection final {
    std::vector<device_identity> devices;
    std::vector<GUID> suppressed_system_guids;
};

// Derive identities from current Core and system evidence. Existing device objects
// keep the exact identity and connection they received at creation.
[[nodiscard]] HRESULT project_catalogue(input_access::call access,
    IDirectInput8W& system,
    catalogue_projection& out) noexcept;
[[nodiscard]] HRESULT find_projected_device(input_access::call access,
    IDirectInput8W& system,
    REFGUID instance,
    device_identity& out) noexcept;
[[nodiscard]] HRESULT discover_projected_device(input_access& access,
    IDirectInput8W& system,
    REFGUID instance,
    device_identity& out) noexcept;

[[nodiscard]] bool is_game_controller_type(DWORD device_type) noexcept;
[[nodiscard]] bool is_game_controller_request(DWORD device_type) noexcept;
[[nodiscard]] bool instance_matches_type(const DIDEVICEINSTANCEW& instance,
    DWORD device_type) noexcept;
[[nodiscard]] bool instance_matches_flags(const DIDEVICEINSTANCEW& instance, DWORD flags) noexcept;
} // namespace inputnext::dinput
