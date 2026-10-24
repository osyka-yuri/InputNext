#pragma once

#include <optional>
#include <stdexcept>

namespace inputnext::test {

template <typename Value>
[[nodiscard]] Value required_value(const std::optional<Value>& value) {
    if (!value.has_value()) {
        throw std::runtime_error{"Test expected a present optional value"};
    }
    return *value;
}

} // namespace inputnext::test
