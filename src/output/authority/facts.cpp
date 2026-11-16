#include "state.hpp"

#include "output/composition/window_acquisition.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <system_error>

namespace inputnext::output {

std::optional<focus::foreground_epoch> authority::implementation::fresh_epoch() const {
    const auto guard = focus_observer.lock_presentation();
    return guard.current_foreground_epoch();
}

bool authority::implementation::window_qualifies(
    const std::shared_ptr<const detail::window_acquisition>& window) const noexcept {
    if (!window || !window->live()) {
        return false;
    }
    try {
        const auto guard = focus_observer.lock_presentation();
        return window->live() && guard.matches_foreground_window_epoch(window->epoch());
    } catch (const std::system_error&) {
        return false;
    }
}

result<std::optional<focus::foreground_epoch>> authority::implementation::prepare_internal_open(
    connection_target target,
    const detail::internal_grant& grant) {
    if (grant.options.foreground != foreground_policy::foreground_bound) {
        return std::optional<focus::foreground_epoch>{};
    }

    const auto before = fresh_epoch();
    if (grant.window &&
        (!before || *before != grant.window->epoch().process || !window_qualifies(grant.window))) {
        return std::unexpected{error::revoked};
    }
    static_cast<void>(close_bound_mismatches(target, before, close_reason::foreground_revocation));
    if (!before) {
        return std::unexpected{error::revoked};
    }
    if (grant.window) {
        static_cast<void>(close_window_mismatches(target, close_reason::foreground_revocation));
    }
    return before;
}

result<void> authority::implementation::revalidate_internal_open(connection_target target,
    const detail::internal_grant& grant,
    std::optional<focus::foreground_epoch> before) {
    if (grant.window) {
        if (!window_qualifies(grant.window)) {
            return std::unexpected{error::revoked};
        }
        return {};
    }
    if (grant.options.foreground != foreground_policy::foreground_bound) {
        return {};
    }
    if (fresh_epoch() == before) {
        return {};
    }
    static_cast<void>(
        close_bound_mismatches(target, std::nullopt, close_reason::foreground_revocation));
    return std::unexpected{error::revoked};
}

inputnext::result<read_pin> authority::implementation::capture(connection_target target) const {
    return registry.capture(target);
}

result<void> authority::implementation::qualify_mutation(record& entry,
    connection_target target,
    std::optional<focus::foreground_epoch> expected) {
    std::optional<focus::foreground_epoch> before;
    if (entry.window_binding) {
        if (entry.window_binding->target() != target || !expected ||
            entry.window_binding->epoch().process != *expected ||
            !window_qualifies(entry.window_binding)) {
            static_cast<void>(close_record(entry, close_reason::foreground_revocation));
            return std::unexpected{error::revoked};
        }
    } else if (entry.foreground == foreground_policy::foreground_bound) {
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

    if (entry.window_binding) {
        if (!window_qualifies(entry.window_binding)) {
            pin->release();
            static_cast<void>(close_record(entry, close_reason::foreground_revocation));
            return std::unexpected{error::revoked};
        }
    } else if (entry.foreground == foreground_policy::foreground_bound) {
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
