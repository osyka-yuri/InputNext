#pragma once

#include <cstdint>
#include <expected>

namespace inputnext {

enum class error : std::uint8_t {
    no_data,
    not_connected,
    invalid_argument,
    capacity_exceeded,
    identity_exhausted,
    revision_exhausted,
    closed,
};

template <typename T>
using result = std::expected<T, error>;

} // namespace inputnext
