#include "history_status.hpp"

#include "core.hpp"
#include <inputnext/history.hpp>

namespace inputnext::core::detail {

status map_history_failure(const history_error error) noexcept {
    switch (error) {
    case history_error::invalid_argument:
        return status::invalid_argument;
    case history_error::invalid_reader:
        return status::stale_reader;
    case history_error::closed:
        return status::unavailable;
    case history_error::disconnected:
        return status::source_retired;
    case history_error::busy:
        return status::busy;
    case history_error::capacity_exceeded:
        return status::capacity_exceeded;
    case history_error::identity_exhausted:
    case history_error::sequence_exhausted:
        return status::unavailable;
    case history_error::no_data:
        return status::no_data;
    case history_error::history_lost:
    case history_error::provider_failure:
    case history_error::clock_discontinuity:
        return status::io_failed;
    }
    return status::io_failed;
}

} // namespace inputnext::core::detail
