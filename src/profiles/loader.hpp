#pragma once

#include "parser.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>

namespace inputnext::profiles {

enum class load_error : std::uint8_t {
    invalid_path,
    not_found,
    io_error,
    too_large,
    invalid_toml,
};

struct load_failure final {
    load_error code;
    diagnostic detail;
};

using load_result = std::expected<parsed_profile, load_failure>;

[[nodiscard]] load_result load_file(const std::filesystem::path& path);
[[nodiscard]] load_result load_for_executable(const std::filesystem::path& executable);

} // namespace inputnext::profiles
