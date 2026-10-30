#include "abi_expected_exports.hpp"
#include "pe_exports.hpp"

#include <Windows.h>
#include <cstdio>
#include <exception>

#include <filesystem>
#include <iostream>
#include <span>

namespace {

[[nodiscard]] bool check_exports(const std::filesystem::path& path,
    const std::span<const inputnext::xinput_dll_tests::expected_export> exports) {
    const auto error = inputnext::xinput_dll_tests::verify_pe_exports(path, exports);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool check_gameinput(const std::filesystem::path& directory) {
    const auto library = directory / L"GameInputRedist.dll";
    HMODULE const module = LoadLibraryExW(library.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        std::cerr << "GameInputRedist load failed: " << GetLastError() << '\n';
        return false;
    }
    const bool released = FreeLibrary(module) != FALSE;
    DWORD binary_type{};
    const auto proxy = directory / L"GameInputRawInputProxy.exe";
    if (!released || GetBinaryTypeW(proxy.c_str(), &binary_type) == FALSE) {
        std::cerr << "GameInput proxy is not a readable executable\n";
        return false;
    }
#ifdef _WIN64
    constexpr DWORD expected_type = SCS_64BIT_BINARY;
#else
    constexpr DWORD expected_type = SCS_32BIT_BINARY;
#endif
    if (binary_type != expected_type) {
        std::cerr << "GameInput proxy architecture does not match the package\n";
        return false;
    }
    return true;
}

} // namespace

int wmain(const int argc, wchar_t** argv) try {
    if (argc != 2 || !std::filesystem::path{argv[1]}.is_absolute()) {
        std::cerr << "Usage: inputnext_distribution_probe <absolute-bin-directory>\n";
        return 2;
    }
    using namespace inputnext::xinput_dll_tests;
    const std::filesystem::path directory{argv[1]};
    if (!check_exports(directory / L"inputnext_runtime.dll", runtime_exports) ||
        !check_exports(directory / L"xinput1_3.dll", xinput13_exports) ||
        !check_exports(directory / L"xinput1_4.dll", xinput14_exports) ||
        !check_exports(directory / L"xinput9_1_0.dll", xinput910_exports) ||
        !check_gameinput(directory)) {
        return 1;
    }
    std::cout << "Package exports and architecture verified\n";
    return 0;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "distribution_probe_error=%s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("distribution_probe_error=unknown exception\n", stderr));
    return 1;
}
