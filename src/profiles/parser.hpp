#pragma once

#include "model.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::profiles {

inline constexpr std::size_t maximum_profile_bytes = std::size_t{64U} * std::size_t{1024U};

struct diagnostic final {
    std::string message;
    std::uint32_t line{};
    std::uint32_t column{};
};

struct parsed_profile final {
    profile settings;
    std::vector<diagnostic> diagnostics;
};

using parse_result = std::expected<parsed_profile, diagnostic>;

[[nodiscard]] parse_result parse_profile(std::string_view content);

} // namespace inputnext::profiles
