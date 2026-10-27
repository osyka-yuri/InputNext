#pragma once

#include <cstdint>

namespace inputnext::runtime_detail {

class native_session;

enum class session_access : std::uint8_t {
    ready,
    initialize_ready,
    retained,
    initialize_retained,
};

// Borrows the process-retained session; explicit shutdown still requires quiescent callers.
[[nodiscard]] native_session* session_for(session_access policy) noexcept;

} // namespace inputnext::runtime_detail
