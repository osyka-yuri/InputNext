#include "session_access.hpp"

#include "runtime/bootstrap/bootstrap_owner.hpp"
#include "runtime/session/native_session.hpp"
#include "runtime_api_internal.hpp"

namespace inputnext::runtime_detail {

native_session* session_for(const session_access policy) noexcept {
    auto* const owner = runtime_bootstrap_owner();
    if (owner == nullptr) {
        return nullptr;
    }
    if (policy == session_access::initialize_ready ||
        policy == session_access::initialize_retained) {
        static_cast<void>(owner->bootstrap());
    }
    const auto state = owner->state();
    const bool allow_degraded =
        policy == session_access::retained || policy == session_access::initialize_retained;
    return state == bootstrap_state::ready || (allow_degraded && state == bootstrap_state::degraded)
               ? owner->session_if_published()
               : nullptr;
}

} // namespace inputnext::runtime_detail
