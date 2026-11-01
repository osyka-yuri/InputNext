#include "support.hpp"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] int run_companion(const std::filesystem::path& bundle) {
    constexpr std::array<const wchar_t*, 2U> names{L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const auto* const name : names) {
        require(GetModuleHandleW(L"xinput1_4.dll") == nullptr,
            "companion scenario started with an existing XInput 1.4 owner");
        module_handle facade = load_library(bundle / name);
        require(facade.get() != nullptr, "could not load a facade with its companion");
        HMODULE const companion = imported_companion(facade.get());
        require(companion != nullptr && module_path_matches(companion, bundle / L"xinput1_4.dll"),
            "facade did not load the app-local XInput 1.4 dependency");
        require(module_not_loaded() != 0U && runtime_module_count() == 0U,
            "companion loading initialized the runtime before an entry call");
        module_handle named{LoadLibraryW(L"xinput1_4.dll")};
        require(named.get() == companion, "name-only load did not reuse the local companion");
        require(facade.close(), "could not unload the facade");
        require(GetModuleHandleW(L"xinput1_4.dll") == companion,
            "companion did not retain its explicit owner");
        require(named.close(), "could not release the explicit companion owner");
        require(GetModuleHandleW(L"xinput1_4.dll") == nullptr,
            "companion outlived its last facade owner");

        module_handle first = load_library(bundle / L"xinput1_4.dll");
        require(first.get() != nullptr, "could not preload the local companion");
        const HMODULE expected = first.get();
        module_handle reverse = load_library(bundle / name);
        require(reverse.get() != nullptr && imported_companion(reverse.get()) == expected,
            "facade did not reuse the preloaded local companion");
        require(first.close(), "could not release the companion preload");
        require(GetModuleHandleW(L"xinput1_4.dll") == expected,
            "preload release unloaded a facade dependency");
        require(reverse.close(), "could not unload the reverse-order facade");
        require(GetModuleHandleW(L"xinput1_4.dll") == nullptr,
            "reverse-order load leaked its companion");
        require(module_not_loaded() != 0U && runtime_module_count() == 0U,
            "reverse-order loading initialized the runtime");
    }
    std::cout << "xinput_companion_load=passed\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
