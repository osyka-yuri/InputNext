#pragma once

#include "runtime/abi/xinput/api.hpp"

namespace inputnext::xinput_facade_detail {

[[nodiscard]] const input_next_runtime_api_v1* runtime_api() noexcept;

} // namespace inputnext::xinput_facade_detail
