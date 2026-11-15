#pragma once

#include <inputnext/device_identity.hpp>

#include <array>
#include <cstdint>

#include <Windows.h>

namespace inputnext::dinput::catalogue_identity {

[[nodiscard]] bool fingerprint_guid(const std::array<std::uint8_t, 32U>& fingerprint, GUID& result);
[[nodiscard]] bool connection_guid(const connection_target& target, GUID& result);

} // namespace inputnext::dinput::catalogue_identity
