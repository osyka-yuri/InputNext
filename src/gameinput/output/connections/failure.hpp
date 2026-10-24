#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

#include <optional>

namespace inputnext::gameinput_detail::connections_failure {

[[nodiscard]] inline connection_control_failure
metadata_control_failure(const connection_target target, const metadata_error failure) noexcept {
    return connection_control_failure{.operation = connection_control_operation::qualify_backend,
        .target = target,
        .metadata_failure = failure};
}

[[nodiscard]] inline connection_control_failure runtime_control_failure(
    const connection_control_operation operation,
    const connection_target target,
    const gameinput_failure failure) noexcept {
    return connection_control_failure{.operation = operation,
        .target = target,
        .backend = std::nullopt,
        .runtime_failure = failure,
        .native_failure = std::nullopt,
        .executor_failure = std::nullopt};
}

[[nodiscard]] inline connection_control_failure native_control_failure(
    const connection_control_operation operation,
    const connection_target target,
    const native_failure failure) noexcept {
    return connection_control_failure{.operation = operation,
        .target = target,
        .backend = std::nullopt,
        .runtime_failure = std::nullopt,
        .native_failure = failure,
        .executor_failure = std::nullopt};
}

[[nodiscard]] inline connection_control_failure executor_control_failure(
    const connection_control_operation operation,
    const connection_target target,
    const std::optional<output::backend_handle> backend,
    const output::execution_error failure) noexcept {
    return connection_control_failure{.operation = operation,
        .target = target,
        .backend = backend,
        .runtime_failure = std::nullopt,
        .native_failure = std::nullopt,
        .executor_failure = failure};
}

[[nodiscard]] inline connection_control_failure protocol_failure(
    const connection_control_operation operation,
    const connection_target target,
    const std::optional<output::backend_handle> backend) noexcept {
    return connection_control_failure{.operation = operation,
        .target = target,
        .backend = backend,
        .runtime_failure = std::nullopt,
        .native_failure = std::nullopt,
        .executor_failure = std::nullopt};
}

} // namespace inputnext::gameinput_detail::connections_failure
