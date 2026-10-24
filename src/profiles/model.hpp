#pragma once

#include <inputnext/output.hpp>

#include <optional>

namespace inputnext::profiles {

struct profile final {
    std::optional<output::trigger_value> left_trigger;
    std::optional<output::trigger_value> right_trigger;

    friend bool operator==(const profile&, const profile&) = default;
};

} // namespace inputnext::profiles
