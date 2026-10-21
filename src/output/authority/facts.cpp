#include "state.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <expected>
#include <optional>

namespace inputnext::output {

std::optional<focus::foreground_epoch> authority::implementation::fresh_epoch() const {
    const auto guard = focus_observer.lock_presentation();
    return guard.current_foreground_epoch();
}

inputnext::result<read_pin> authority::implementation::capture(connection_target target) const {
    return registry.capture(target);
}

result<void> authority::implementation::qualify_mutation(record& entry,
    connection_target target,
    std::optional<focus::foreground_epoch> expected) {
    std::optional<focus::foreground_epoch> before;
    if (entry.foreground == foreground_policy::foreground_bound) {
        before = fresh_epoch();
        if (!before.has_value() || before != expected) {
            static_cast<void>(close_record(entry, close_reason::foreground_revocation));
            return std::unexpected{error::revoked};
        }
    }

    auto pin = capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            static_cast<void>(retire(target));
            return std::unexpected{error::not_connected};
        }
        return std::unexpected{error::resources};
    }

    if (entry.foreground == foreground_policy::foreground_bound) {
        const auto after = fresh_epoch();
        if (!after.has_value() || after != before || after != expected) {
            pin->release();
            static_cast<void>(close_record(entry, close_reason::foreground_revocation));
            return std::unexpected{error::revoked};
        }
    }
    pin->release();
    return {};
}

} // namespace inputnext::output
