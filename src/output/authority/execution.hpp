#pragma once

#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

namespace inputnext::output::detail {

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
};

} // namespace inputnext::output::detail
