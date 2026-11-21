#pragma once

#include "profiles/model.hpp"
#include "profiles/parser.hpp"

#include <toml++/toml.hpp>

#include <vector>

namespace inputnext::profiles::detail {

[[nodiscard]] directinput_profile parse_directinput(const toml::node& source,
    std::vector<diagnostic>& diagnostics);

} // namespace inputnext::profiles::detail
