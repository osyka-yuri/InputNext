#pragma once

#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>

namespace inputnext::output::detail {

enum class completion_disposition : std::uint8_t {
    stale,
    current,
    fallback_committed,
};

class authority_execution_access final {
public:
    [[nodiscard]] static execution_result<execution_owner> attach(authority& owner);
    [[nodiscard]] static execution_result<void> detach(authority& owner,
        execution_owner expected_owner);
    [[nodiscard]] static execution_result<void>
    check_target(authority& owner, execution_owner expected_owner, connection_target target);
    [[nodiscard]] static execution_result<bool> qualifies(authority& owner,
        execution_owner expected_owner,
        const represented_projection& expected);
    [[nodiscard]] static execution_result<bool> qualifies_route(authority& owner,
        execution_owner expected_owner,
        const body_route_intent& expected);
    [[nodiscard]] static execution_result<completion_disposition> complete_route(authority& owner,
        execution_owner expected_owner,
        const body_route_intent& expected,
        native_status status);
};

} // namespace inputnext::output::detail
