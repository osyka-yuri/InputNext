#pragma once

#include "runtime/abi/xinput/api.hpp"

namespace inputnext::runtime_detail {

class bootstrap_owner;
struct query_front;

[[nodiscard]] const input_next_runtime_api_v1* runtime_api_v1() noexcept;
[[nodiscard]] bootstrap_owner* runtime_bootstrap_owner() noexcept;
[[nodiscard]] query_front& runtime_query_front() noexcept;

} // namespace inputnext::runtime_detail
