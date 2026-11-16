#include "composition.hpp"

#include "internal_producer.hpp"
#include "output/authority/internal_grant.hpp"
#include "output/authority/state.hpp"
#include "output/body/audio_route_control.hpp"
#include "output/composition/window_acquisition.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace inputnext::output::detail {

composition::composition(runtime_id runtime,
    device_registry& registry,
    focus::observer& focus_observer,
    authority_seed seed)
    : owner_(runtime, registry, focus_observer, seed) {
}

authority& composition::public_authority() & noexcept {
    return owner_;
}

audio_route_control composition::audio_routes() & noexcept {
    return audio_route_control{owner_};
}

result<internal_producer> composition::make_producer(
    const internal_context_options& options) & noexcept {
    const auto valid = policy_detail::validate_internal_context_options(options);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }
    return internal_producer{owner_,
        internal_grant{.options = options, .window = nullptr, .backend = std::nullopt}};
}

result<internal_producer> composition::make_window_rumble(
    std::shared_ptr<const window_acquisition> window,
    const std::optional<backend_handle> backend) & noexcept {
    if (!window || !window->target().valid() ||
        !window->epoch().process.observer_incarnation.valid() ||
        window->epoch().process.process.pid == 0U ||
        window->epoch().process.process.creation_time_100ns == 0U ||
        window->epoch().process.revision == 0U || window->epoch().window == 0U ||
        window->epoch().revision == 0U || !window->live() ||
        (backend.has_value() && (!backend->valid() || backend->target != window->target()))) {
        return std::unexpected{error::invalid_argument};
    }
    if (window->target().runtime != owner_.implementation_->runtime) {
        return std::unexpected{error::not_connected};
    }
    internal_context_options options{.priority = internal_priority::game_semantic,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::foreground_bound};
    try {
        std::scoped_lock lock{owner_.implementation_->gate};
        if (backend.has_value() &&
            (!owner_.implementation_->attached_execution.has_value() ||
                backend->incarnation.owner != *owner_.implementation_->attached_execution)) {
            return std::unexpected{error::not_connected};
        }
        const auto owner_id = owner_.implementation_->take_context_id();
        if (!owner_id.has_value()) {
            return std::unexpected{owner_id.error()};
        }
        options.owner = internal_owner_key{*owner_id};
        if (!policy_detail::validate_internal_context_options(options).has_value()) {
            return std::unexpected{error::invalid_argument};
        }
        return internal_producer{owner_,
            internal_grant{.options = options, .window = std::move(window), .backend = backend}};
    } catch (const std::system_error&) {
        return std::unexpected{error::resources};
    }
}

} // namespace inputnext::output::detail
