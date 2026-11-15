#pragma once

#include "dinput/native_types.hpp"

#include <memory>

namespace inputnext::dinput {

class input_access;
struct device_identity;

// The returned A/W views share one identity and retain their parent and access block.
[[nodiscard]] HRESULT make_device(const device_identity& identity,
    std::shared_ptr<input_access> access,
    IUnknown& parent,
    bool unicode,
    void** out) noexcept;

} // namespace inputnext::dinput
