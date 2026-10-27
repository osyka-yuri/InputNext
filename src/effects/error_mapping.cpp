#include "error_mapping.hpp"

#include "core/core.hpp"
#include "effects/effects.hpp"

#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

namespace inputnext::effects {

status map_core_error(const core::status value) noexcept {
    switch (value) {
    case core::status::ok:
        return status::ok;
    case core::status::invalid_argument:
        return status::invalid_argument;
    case core::status::unavailable:
        return status::unavailable;
    case core::status::stale_target:
        return status::stale_target;
    case core::status::busy:
        return status::busy;
    case core::status::capacity_exceeded:
        return status::capacity_exceeded;
    case core::status::source_retired:
        return status::source_retired;
    case core::status::io_failed:
        return status::io_failed;
    case core::status::no_data:
        return status::no_data;
    case core::status::ambiguous_target:
        return status::unsupported_target;
    case core::status::not_admitted:
        return status::not_admitted;
    case core::status::buffer_too_small:
    case core::status::stale_reader:
        return status::invalid_argument;
    }
    return status::unavailable;
}

status map_execution_error(const output::execution_error value) noexcept {
    switch (value) {
    case output::execution_error::invalid_argument:
        return status::invalid_argument;
    case output::execution_error::not_connected:
    case output::execution_error::not_running:
    case output::execution_error::stale_backend:
        return status::stale_target;
    case output::execution_error::capacity_exceeded:
    case output::execution_error::identity_exhausted:
    case output::execution_error::resources:
        return status::capacity_exceeded;
    case output::execution_error::already_attached:
    case output::execution_error::backend_conflict:
    case output::execution_error::busy:
        return status::busy;
    case output::execution_error::pending:
        return status::scheduler_unavailable;
    }
    return status::io_failed;
}

status map_gameinput_error(const gameinput_error value) noexcept {
    switch (value) {
    case gameinput_error::invalid_argument:
        return status::invalid_argument;
    case gameinput_error::resource_exhausted:
        return status::capacity_exceeded;
    case gameinput_error::closed:
        return status::unavailable;
    case gameinput_error::already_started:
        return status::busy;
    case gameinput_error::load_failed:
    case gameinput_error::api_unavailable:
    case gameinput_error::registration_failed:
    case gameinput_error::native_failure:
        return status::io_failed;
    }
    return status::io_failed;
}

status map_metadata_error(const metadata_error value) noexcept {
    switch (value) {
    case metadata_error::closed:
        return status::unavailable;
    case metadata_error::not_connected:
        return status::stale_target;
    case metadata_error::busy:
    case metadata_error::stale:
        return status::busy;
    case metadata_error::resource_exhausted:
        return status::capacity_exceeded;
    }
    return status::io_failed;
}

status map_output_error(const output::error value) noexcept {
    switch (value) {
    case output::error::invalid_argument:
    case output::error::deadline_overflow:
        return status::invalid_argument;
    case output::error::not_connected:
        return status::stale_target;
    case output::error::resources:
    case output::error::capacity_exceeded:
    case output::error::identity_exhausted:
    case output::error::revision_exhausted:
    case output::error::logical_version_exhausted:
        return status::capacity_exceeded;
    case output::error::sequence_rejected:
        return status::stale_sequence;
    case output::error::stale_context:
        return status::stale_context;
    case output::error::revoked:
        return status::revoked;
    case output::error::stale_revision:
        return status::stale_effect;
    case output::error::expired:
        return status::not_admitted;
    case output::error::busy:
        return status::busy;
    case output::error::stale_lease:
    case output::error::stale_audio:
        return status::stale_lease;
    }
    return status::io_failed;
}

} // namespace inputnext::effects
