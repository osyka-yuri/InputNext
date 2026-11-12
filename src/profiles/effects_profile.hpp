#pragma once

#include <inputnext/output.hpp>

#include <optional>

namespace inputnext::profiles {

struct effects_profile final {
    std::optional<output::trigger_value> left_trigger = std::nullopt;
    std::optional<output::trigger_value> right_trigger = std::nullopt;

    friend bool operator==(const effects_profile&, const effects_profile&) = default;
};

} // namespace inputnext::profiles
