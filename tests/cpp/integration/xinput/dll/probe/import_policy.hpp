#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] inline bool import_name_contains(const std::string_view name,
    const std::string_view fragment) noexcept {
    return !std::ranges::search(name, fragment, [](const char left, const char right) {
        return std::tolower(static_cast<unsigned char>(left)) == right;
    }).empty();
}

[[nodiscard]] inline bool runtime_dll_import_allowed(const std::string_view name,
    const bool address_profile) noexcept {
    if (import_name_contains(name, "ubsan")) {
        return false;
    }
    if (import_name_contains(name, "asan")) {
        const auto matches = [name](const std::string_view expected) {
            return name.size() == expected.size() && import_name_contains(name, expected);
        };
        return address_profile && (matches("clang_rt.asan_dynamic-x86_64.dll") ||
                                      matches("clang_rt.asan_dynamic-i386.dll"));
    }
    return !import_name_contains(name, "clang_rt.");
}

[[nodiscard]] inline bool runtime_symbol_import_allowed(const std::string_view name,
    const bool address_profile) noexcept {
    return !name.starts_with("__ubsan_") && !name.contains("@__ubsan@@") &&
           (address_profile || (!name.starts_with("__asan_") && !name.starts_with("__sanitizer_")));
}

} // namespace inputnext::xinput_dll_probe
