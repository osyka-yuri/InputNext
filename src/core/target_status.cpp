#include "target_status.hpp"

#include "core.hpp"
#include <inputnext/error.hpp>

namespace inputnext::core::detail {

status map_registry_error(const error failure) noexcept {
    switch (failure) {
    case error::invalid_argument:
        return status::invalid_argument;
    case error::not_connected:
        return status::stale_target;
    case error::no_data:
        return status::no_data;
    case error::capacity_exceeded:
        return status::capacity_exceeded;
    case error::identity_exhausted:
    case error::revision_exhausted:
    case error::closed:
        return status::unavailable;
    }
    return status::io_failed;
}

} // namespace inputnext::core::detail
