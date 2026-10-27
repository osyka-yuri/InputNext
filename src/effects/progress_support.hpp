#pragma once

#include "effects.hpp"

#include <inputnext/output_execution.hpp>

namespace inputnext::effects {

[[nodiscard]] inline status delivery_failure(const output::backend_progress& progress) noexcept {
    if (progress.delivery == output::delivery_state::identity_exhausted) {
        return status::capacity_exceeded;
    }
    if (progress.delivery != output::delivery_state::blocked) {
        return status::pending;
    }

    if (progress.write_in_flight) {
        return status::pending;
    }
    if (!progress.observed_publication.valid() || !progress.last_attempt.has_value() ||
        !progress.last_completed_attempt.has_value()) {
        return status::io_failed;
    }

    const auto& attempt = *progress.last_completed_attempt;
    if (!attempt.id.valid() || *progress.last_attempt != attempt.id ||
        attempt.publication != progress.observed_publication) {
        return status::io_failed;
    }

    switch (attempt.result.status) {
    case output::native_status::unsupported:
        return status::unsupported;
    case output::native_status::accepted:
    case output::native_status::retryable_failure:
    case output::native_status::failed:
    case output::native_status::disconnected:
        return status::io_failed;
    }
    return status::io_failed;
}

} // namespace inputnext::effects
