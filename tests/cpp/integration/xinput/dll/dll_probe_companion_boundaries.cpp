#include "dll_probe_support.hpp"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>

namespace inputnext::xinput_dll_probe {
namespace {

[[nodiscard]] std::filesystem::path system_companion_path() {
    std::array<wchar_t, MAX_PATH> directory{};
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    require(length != 0U && length < directory.size(), "could not resolve System32");
    return std::filesystem::path{directory.data()} / L"xinput1_4.dll";
}

[[nodiscard]] module_handle load_system_companion() {
    const auto path = system_companion_path();
    module_handle system = load_library(path);
    require(system.get() != nullptr, "could not load the supported Windows XInput 1.4");
    require(module_path_matches(system.get(), path), "system companion resolved to another path");
    return system;
}

} // namespace

[[nodiscard]] int run_companion_preloaded(const std::filesystem::path& bundle) {
    require(GetModuleHandleW(L"xinput1_4.dll") == nullptr,
        "preload scenario started with an existing companion");
    auto system = load_system_companion();
    require(!module_path_matches(system.get(), bundle / L"xinput1_4.dll"),
        "system preload resolved to the local companion");
    constexpr std::array<const wchar_t*, 2U> names{L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const auto* const name : names) {
        module_handle facade = load_library(bundle / name);
        require(facade.get() != nullptr, "could not load the facade after system preload");
        require(imported_companion(facade.get()) == system.get(),
            "facade replaced the preloaded system companion");
        require(facade.close(), "could not unload the facade after system preload");
        require(GetModuleHandleW(name) == nullptr, "system preload retained the importing facade");
        require(GetModuleHandleW(L"xinput1_4.dll") == system.get(),
            "facade unload released the external system owner");
    }
    require(system.close(), "could not release the system preload");
    require(module_not_loaded() != 0U, "system preload started the runtime");
    std::cout << "xinput_companion_preloaded=passed\n";
    return 0;
}

[[nodiscard]] int run_companion_missing(const std::filesystem::path& directory) {
    require(!std::filesystem::exists(directory / L"xinput1_4.dll"),
        "missing-companion fixture contains a local companion");
    require(GetModuleHandleW(L"xinput1_4.dll") == nullptr,
        "missing-companion scenario started with an existing companion");
    constexpr std::array<const wchar_t*, 2U> names{L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const auto* const name : names) {
        module_handle facade = load_library(directory / name);
        require(facade.get() != nullptr, "Windows did not resolve the missing companion");
        HMODULE const companion = imported_companion(facade.get());
        auto system = load_system_companion();
        require(companion == system.get(), "missing companion did not resolve to Windows XInput");
        require(system.close() && facade.close(), "could not release the fallback owners");
        require(GetModuleHandleW(name) == nullptr, "fallback retained the importing facade");
    }
    require(module_not_loaded() != 0U, "missing-companion loading started the runtime");
    std::cout << "xinput_companion_missing=passed\n";
    return 0;
}

[[nodiscard]] int run_companion_system_path(const std::filesystem::path& bundle) {
    const auto system_path = system_companion_path();
    module_handle facade = load_library(bundle / L"xinput1_3.dll");
    require(facade.get() != nullptr, "could not load the local facade");
    HMODULE const local = imported_companion(facade.get());
    require(local != nullptr && module_path_matches(local, bundle / L"xinput1_4.dll"),
        "system-path scenario did not start with the local companion");
    module_handle system{LoadLibraryW(system_path.c_str())};
    require(system.get() != nullptr && system.get() != local &&
                module_path_matches(system.get(), system_path),
        "absolute system path did not load a separate companion");
    require(system.close() && facade.close(), "could not release the system-path owners");
    require(GetModuleHandleW((bundle / L"xinput1_4.dll").c_str()) == nullptr &&
                GetModuleHandleW(L"xinput1_3.dll") == nullptr,
        "system-path load retained a local facade");
    require(module_not_loaded() != 0U, "absolute system load started the runtime");
    std::cout << "xinput_companion_system_path=passed\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
