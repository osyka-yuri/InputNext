#pragma once

#include "native_types.hpp"
#include <inputnext/device_identity.hpp>

#include <optional>

namespace inputnext::dinput {

struct object_descriptor;

struct device_identity final {
    connection_target target{};
    DIDEVICEINSTANCEW instance{};
    std::optional<DIPROPGUIDANDPATH> path;
    std::optional<DWORD> vendor_product;
};

[[nodiscard]] HRESULT copy_instance_ansi(const DIDEVICEINSTANCEW& source,
    DIDEVICEINSTANCEA& destination) noexcept;
[[nodiscard]] DIDEVICEOBJECTINSTANCEW describe_object(const object_descriptor& object) noexcept;
[[nodiscard]] DIDEVICEOBJECTINSTANCEA describe_object_ansi(
    const object_descriptor& object) noexcept;
[[nodiscard]] bool valid_object_filter(DWORD flags) noexcept;
[[nodiscard]] bool matches_object_filter(const object_descriptor& object, DWORD flags) noexcept;

} // namespace inputnext::dinput
