#pragma once

#include "core.hpp"
#include <inputnext/error.hpp>

namespace inputnext::core::detail {

[[nodiscard]] status map_registry_error(error failure) noexcept;

} // namespace inputnext::core::detail
