#include "core_state.hpp"

#include "core.hpp"
#include "target_status.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/input_binding.hpp>

#include <atomic>
#include <expected>
#include <new>

namespace inputnext::core {

result<connection_snapshot> service::implementation::read_latest(
    const connection_target target) const {
    if (!target.valid()) {
        return std::unexpected(status::invalid_argument);
    }
    if (target.runtime != identity) {
        return std::unexpected(status::stale_target);
    }
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    try {
        auto snapshot = registry.read(target);
        if (!snapshot.has_value()) {
            return std::unexpected(detail::map_registry_error(snapshot.error()));
        }
        if (snapshot->target != target) {
            return std::unexpected(status::stale_target);
        }
        if (admission_closed.load(std::memory_order_acquire)) {
            return std::unexpected(status::unavailable);
        }
        return *snapshot;
    } catch (const std::bad_alloc&) {
        return std::unexpected(status::capacity_exceeded);
    } catch (...) {
        return std::unexpected(status::io_failed);
    }
}

} // namespace inputnext::core
