#include "loader.hpp"

#include "detail/profile_stream.hpp"
#include "parser.hpp"

#include <share.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>

namespace inputnext::profiles {
namespace {

[[nodiscard]] load_failure failure(const load_error code, std::string message) {
    return load_failure{
        .code = code,
        .detail = diagnostic{.message = std::move(message)},
    };
}

[[nodiscard]] load_error classify_path_error(const std::error_code& error) noexcept {
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return load_error::not_found;
    }
    if (error == std::errc::invalid_argument || error == std::errc::filename_too_long) {
        return load_error::invalid_path;
    }
    return load_error::io_error;
}

[[nodiscard]] load_error classify_open_failure(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error) {
        return classify_path_error(error);
    }
    return std::filesystem::exists(status) ? load_error::io_error : load_error::not_found;
}

} // namespace

load_result load_file(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(failure(load_error::invalid_path, "Profile path is empty"));
    }

    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error) {
        return std::unexpected(
            failure(classify_path_error(error), "Could not inspect profile file"));
    }
    if (!std::filesystem::exists(status)) {
        return std::unexpected(failure(load_error::not_found, "Profile file was not found"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(failure(load_error::io_error, "Profile path is not a regular file"));
    }

    // Keep ordinary writers out until the owned read finishes.
    std::ifstream file(path, std::ios::binary, _SH_DENYWR);
    if (!file.is_open()) {
        return std::unexpected(failure(classify_open_failure(path), "Could not open profile file"));
    }

    file.seekg(0, std::ios::end);
    const auto end_position = file.tellg();
    const auto initial_size_offset = static_cast<std::streamoff>(end_position);
    if (initial_size_offset < 0) {
        return std::unexpected(
            failure(load_error::io_error, "Could not inspect profile file size"));
    }
    const auto initial_size = static_cast<std::uintmax_t>(initial_size_offset);
    if (initial_size > maximum_profile_bytes) {
        return std::unexpected(
            failure(load_error::too_large, "Profile file exceeds the 64 KiB size limit"));
    }

    file.seekg(0, std::ios::beg);
    if (!file) {
        return std::unexpected(
            failure(load_error::io_error, "Could not seek to start of profile file"));
    }

    auto content = detail::read_profile_stream(file, initial_size);
    if (!content) {
        return std::unexpected(std::move(content.error()));
    }

    auto parsed = parse_profile(*content);
    if (!parsed) {
        return std::unexpected(load_failure{
            .code = load_error::invalid_toml,
            .detail = std::move(parsed.error()),
        });
    }
    return std::move(*parsed);
}

load_result load_for_executable(const std::filesystem::path& executable) {
    if (executable.empty() || !executable.is_absolute() || executable.filename().empty()) {
        return std::unexpected(failure(load_error::invalid_path,
            "Executable path must be non-empty, absolute, and name an executable"));
    }
    return load_file(executable.parent_path() / L"inputnext.toml");
}

} // namespace inputnext::profiles
