#include "profile_stream.hpp"

#include "profiles/loader.hpp"
#include "profiles/parser.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ios>
#include <istream>
#include <string>
#include <utility>

namespace inputnext::profiles::detail {
namespace {

[[nodiscard]] load_failure failure(const load_error code, std::string message) {
    return load_failure{
        .code = code,
        .detail = diagnostic{.message = std::move(message)},
    };
}

} // namespace

stream_read_result read_profile_stream(std::istream& stream, const std::uintmax_t initial_size) {
    if (initial_size > maximum_profile_bytes) {
        return std::unexpected(
            failure(load_error::too_large, "Profile file exceeds the 64 KiB size limit"));
    }

    std::string content;
    content.reserve(static_cast<std::size_t>(initial_size));
    std::array<char, 4096U> buffer{};
    for (;;) {
        const auto remaining = maximum_profile_bytes + 1U - content.size();
        const auto request = std::min(buffer.size(), remaining);
        stream.read(buffer.data(), static_cast<std::streamsize>(request));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            const auto bytes = static_cast<std::size_t>(count);
            if (bytes > maximum_profile_bytes - content.size()) {
                return std::unexpected(failure(load_error::too_large,
                    "Profile file grew beyond the 64 KiB size limit"));
            }
            content.append(buffer.data(), bytes);
        }

        if (stream.bad()) {
            return std::unexpected(
                failure(load_error::io_error, "Failed while reading profile file"));
        }
        if (stream.eof()) {
            break;
        }
        if (stream.fail() || count == 0) {
            return std::unexpected(
                failure(load_error::io_error, "Failed while reading profile file"));
        }
    }

    if (content.size() < initial_size) {
        return std::unexpected(
            failure(load_error::io_error, "Profile file changed during a partial read"));
    }
    return content;
}

} // namespace inputnext::profiles::detail
