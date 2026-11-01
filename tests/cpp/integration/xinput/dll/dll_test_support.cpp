#include "dll_test_support.hpp"

#include "gameinput_probe/process/supervisor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <Windows.h>

namespace inputnext::xinput_dll_tests {
namespace {

constinit std::atomic<std::uint32_t> directory_sequence{};

[[nodiscard]] std::filesystem::path sibling_path(const wchar_t* name) {
    const auto current = current_executable_path();
    return current.empty() ? std::filesystem::path{} : current.parent_path() / name;
}

} // namespace

temporary_directory::temporary_directory(const wchar_t* const purpose) {
    const auto root = std::filesystem::temp_directory_path();
    const auto process = GetCurrentProcessId();
    for (std::uint32_t attempt = 0U; attempt < 64U; ++attempt) {
        const auto sequence = directory_sequence.fetch_add(1U, std::memory_order_relaxed);
        path_ = root / (std::wstring{L"inputnext-xinput-"} + purpose + L"-" +
                           std::to_wstring(process) + L"-" + std::to_wstring(sequence));
        std::error_code error;
        if (std::filesystem::create_directory(path_, error)) {
            return;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error("create test temporary directory",
                path_,
                error);
        }
    }
    throw std::runtime_error("Could not allocate an isolated XInput test directory.");
}

temporary_directory::~temporary_directory() noexcept {
    if (const auto error = cleanup(); error) {
        OutputDebugStringW(L"InputNext XInput test-directory cleanup failed for: ");
        OutputDebugStringW(path_.c_str());
        OutputDebugStringW(L"\n");
    }
}

std::error_code temporary_directory::cleanup() noexcept {
    if (path_.empty()) {
        return {};
    }
    try {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (!error) {
            path_.clear();
        }
        return error;
    } catch (const std::bad_alloc&) {
        return std::make_error_code(std::errc::not_enough_memory);
    } catch (const std::filesystem::filesystem_error& error) {
        return error.code();
    } catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
}

std::filesystem::path current_executable_path() {
    std::wstring buffer(512U, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32'768U));
    }
}

child_result run_probe(std::vector<std::wstring> arguments,
    const std::chrono::milliseconds timeout,
    std::filesystem::path working_directory) {
    const auto executable = sibling_path(L"inputnext_xinput_dll_probe.exe");
    if (executable.empty()) {
        return {.process = {.timed_out = false, .exit_code = 0xFFFF'FFFFUL},
            .output = "Could not locate the XInput DLL process probe.",
            .errors = {}};
    }
    if (working_directory.empty()) {
        working_directory = executable.parent_path();
    }

    std::ostringstream output;
    std::ostringstream errors;
    const auto process =
        gameinput_detail::run_owned_process({.executable = executable,
                                                .working_directory = std::move(working_directory),
                                                .arguments = std::move(arguments),
                                                .timeout = timeout},
            {.output = output, .errors = errors});
    return {.process = process, .output = output.str(), .errors = errors.str()};
}

child_result run_import_probe(const std::filesystem::path& executable,
    const std::chrono::milliseconds timeout,
    std::filesystem::path working_directory) {
    if (working_directory.empty()) {
        working_directory = executable.parent_path();
    }

    std::ostringstream output;
    std::ostringstream errors;
    const auto process =
        gameinput_detail::run_owned_process({.executable = executable,
                                                .working_directory = std::move(working_directory),
                                                .arguments = {},
                                                .timeout = timeout},
            {.output = output, .errors = errors});
    return {.process = process, .output = output.str(), .errors = errors.str()};
}

void copy_fixture(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::copy_file(source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) {
        throw std::filesystem::filesystem_error("copy XInput test fixture",
            source,
            destination,
            error);
    }
}

} // namespace inputnext::xinput_dll_tests
