#pragma once

#include "profiles/parser.hpp"

#include <toml++/toml.hpp>

#include <string>
#include <utility>

namespace inputnext::profiles::detail {

[[nodiscard]] inline diagnostic located_diagnostic(const toml::node& source, std::string message) {
    const auto position = source.source().begin;
    return diagnostic{
        .message = std::move(message),
        .line = position.line,
        .column = position.column,
    };
}

} // namespace inputnext::profiles::detail
