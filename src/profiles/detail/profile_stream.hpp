#pragma once

#include "profiles/loader.hpp"

#include <cstdint>
#include <expected>
#include <istream>
#include <string>

namespace inputnext::profiles::detail {

using stream_read_result = std::expected<std::string, load_failure>;

[[nodiscard]] stream_read_result read_profile_stream(std::istream& stream,
    std::uintmax_t initial_size);

} // namespace inputnext::profiles::detail
