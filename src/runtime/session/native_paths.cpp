#include "native_paths.hpp"

#include "profiles/loader.hpp"
#include "profiles/model.hpp"
#include "profiles/parser.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace inputnext::runtime_detail {
namespace {

constexpr std::size_t module_path_limit = 32768U;
constexpr wchar_t module_anchor{};

[[nodiscard]] std::filesystem::path module_path(HMODULE module) {
    std::vector<wchar_t> buffer(static_cast<std::size_t>(MAX_PATH));
    for (;;) {
        const auto length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "GetModuleFileNameW");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path{
                std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        }
        if (buffer.size() >= module_path_limit) {
            throw std::length_error{"module path exceeds Windows path limit"};
        }
        buffer.resize(std::min(buffer.size() * 2U, module_path_limit));
    }
}

void report_diagnostic(const profiles::diagnostic& diagnostic) {
    std::string text{"InputNext inputnext.toml"};
    if (diagnostic.line != 0U) {
        text += ':' + std::to_string(diagnostic.line) + ':' + std::to_string(diagnostic.column);
    }
    text += ": " + diagnostic.message + '\n';
    OutputDebugStringA(text.c_str());
}

} // namespace

std::filesystem::path runtime_library_path() {
    HMODULE module{};
    // The anchor selects this runtime module rather than the game's executable.
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            &module_anchor,
            &module) == FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "GetModuleHandleExW");
    }
    return module_path(module).parent_path() / L"GameInputRedist.dll";
}

profiles::profile load_process_profile() {
    std::filesystem::path executable;
    try {
        executable = module_path(nullptr);
    } catch (const std::system_error& error) {
        report_diagnostic({.message = error.what()});
        return {};
    }

    auto loaded = profiles::load_for_executable(executable);
    if (!loaded.has_value()) {
        if (loaded.error().code != profiles::load_error::not_found) {
            report_diagnostic(loaded.error().detail);
        }
        return {};
    }
    for (const auto& diagnostic : loaded->diagnostics) {
        report_diagnostic(diagnostic);
    }
    return loaded->settings;
}

} // namespace inputnext::runtime_detail
