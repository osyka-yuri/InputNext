#pragma once

#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>

namespace inputnext::output {

struct backend_observation final {
    backend_progress progress{};
    std::uint64_t observed_logical_version{};
    logical_snapshot authority_snapshot{};
    bool projection_current{};
};

} // namespace inputnext::output
