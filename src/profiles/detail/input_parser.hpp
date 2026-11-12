#pragma once

#include "input/program.hpp"
#include "profiles/parser.hpp"

#include <toml++/toml.hpp>

#include <vector>

namespace inputnext::profiles::detail {

[[nodiscard]] input::program parse_input(const toml::node& source,
    std::vector<diagnostic>& diagnostics);

} // namespace inputnext::profiles::detail
