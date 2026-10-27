#pragma once

#include "core/core.hpp"
#include "effects.hpp"

#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

namespace inputnext::effects {

[[nodiscard]] status map_core_error(core::status value) noexcept;
[[nodiscard]] status map_execution_error(output::execution_error value) noexcept;
[[nodiscard]] status map_gameinput_error(gameinput_error value) noexcept;
[[nodiscard]] status map_metadata_error(metadata_error value) noexcept;
[[nodiscard]] status map_output_error(output::error value) noexcept;

} // namespace inputnext::effects
