#pragma once

#include "effects.hpp"
#include "gameinput/output/connections/manager.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

namespace inputnext::effects {

[[nodiscard]] status current_backend(gameinput_detail::gameinput_output_connections& connections,
    connection_target target,
    gameinput_detail::managed_backend_observation& out) noexcept;
[[nodiscard]] status current_backend_support(
    gameinput_detail::gameinput_output_connections& connections,
    connection_target target,
    output::channel_mask requested,
    gameinput_detail::managed_backend_observation& out) noexcept;

} // namespace inputnext::effects
