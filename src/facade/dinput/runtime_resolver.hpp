#pragma once

#include "runtime/abi/dinput/api.hpp"

namespace inputnext::dinput_facade_detail {

[[nodiscard]] const input_next_directinput_api_v1* runtime_api() noexcept;

} // namespace inputnext::dinput_facade_detail
