#include "composition.hpp"

#include "internal_producer.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <expected>

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

result<internal_producer> composition::make_producer(
    const internal_context_options& options) & noexcept {
    const auto valid = policy_detail::validate_internal_context_options(options);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }
    return internal_producer{owner_, options};
}

} // namespace inputnext::output::detail
