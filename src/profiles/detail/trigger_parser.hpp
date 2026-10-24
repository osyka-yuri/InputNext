#pragma once

#include "profiles/parser.hpp"
#include <inputnext/output.hpp>

#include <toml++/toml.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::profiles::detail {

struct trigger_parse_result final {
    std::optional<output::trigger_value> value;
    std::vector<diagnostic> diagnostics;
};

[[nodiscard]] diagnostic located_diagnostic(const toml::node& source, std::string message);
[[nodiscard]] trigger_parse_result parse_trigger(const toml::node& source, std::string_view name);

} // namespace inputnext::profiles::detail
