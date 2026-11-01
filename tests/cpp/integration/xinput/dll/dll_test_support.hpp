#pragma once

#include "gameinput_probe/process/supervisor.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace inputnext::xinput_dll_tests {

class temporary_directory final {
public:
    explicit temporary_directory(const wchar_t* purpose);
    ~temporary_directory() noexcept;

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;
    temporary_directory(temporary_directory&&) = delete;
    temporary_directory& operator=(temporary_directory&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::error_code cleanup() noexcept;

private:
    std::filesystem::path path_;
};

struct child_result final {
    gameinput_detail::owned_process_result process{};
    std::string output;
    std::string errors;
};

[[nodiscard]] std::filesystem::path current_executable_path();
[[nodiscard]] child_result run_probe(std::vector<std::wstring> arguments,
    std::chrono::milliseconds timeout,
    std::filesystem::path working_directory = {});
[[nodiscard]] child_result run_import_probe(const std::filesystem::path& executable,
    std::chrono::milliseconds timeout,
    std::filesystem::path working_directory = {});
void copy_fixture(const std::filesystem::path& source, const std::filesystem::path& destination);

} // namespace inputnext::xinput_dll_tests
