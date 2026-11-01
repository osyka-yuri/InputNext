#pragma once

#include <input_next_core.h>

namespace inputnext::runtime_detail {

// Returns the immutable ABI-1 Core function table.
[[nodiscard]] const input_next_core_api_v1* core_api_v1() noexcept;

} // namespace inputnext::runtime_detail
