#pragma once

#include <inputnext/output.hpp>

#include <cstdint>

namespace inputnext::output::detail {

enum class incarnation_creation : std::uint8_t { generate, force_failure_for_test };

[[nodiscard]] authority_incarnation create_authority_incarnation(incarnation_creation creation);

} // namespace inputnext::output::detail
