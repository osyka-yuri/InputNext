#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace inputnext::facade_detail {

inline constexpr DWORD path_limit = 32'768U;
inline constexpr DWORD initial_path_capacity = 512U;

[[nodiscard]] inline bool has_extended_prefix(const std::wstring_view path) noexcept {
    return path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\");
}

[[nodiscard]] inline bool is_absolute_path(const std::wstring_view path) noexcept {
    return (path.size() >= 3U && path[1U] == L':' && (path[2U] == L'\\' || path[2U] == L'/')) ||
           path.starts_with(L"\\\\");
}

[[nodiscard]] inline bool get_path_from_module(const HMODULE module, std::wstring& path) {
    std::vector<wchar_t> buffer(initial_path_capacity);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return false;
        }
        if (length < buffer.size()) {
            path.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= path_limit) {
            return false;
        }
        const auto next_size = buffer.size() * 2U;
        buffer.resize(next_size > path_limit ? path_limit : next_size);
    }
}

[[nodiscard]] inline bool make_absolute_path(std::wstring& path) {
    if (is_absolute_path(path)) {
        return true;
    }

    std::vector<wchar_t> buffer(initial_path_capacity);
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(),
            static_cast<DWORD>(buffer.size()),
            buffer.data(),
            nullptr);
        if (length == 0U) {
            return false;
        }
        if (length < buffer.size()) {
            path.assign(buffer.data(), length);
            return true;
        }
        if (length >= path_limit) {
            return false;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1U);
    }
}

[[nodiscard]] inline std::wstring with_extended_prefix(std::wstring path) {
    if (has_extended_prefix(path)) {
        return path;
    }
    if (path.starts_with(L"\\\\")) {
        return std::wstring{L"\\\\?\\UNC\\"} + path.substr(2U);
    }
    return std::wstring{L"\\\\?\\"} + path;
}

[[nodiscard]] inline bool sibling_runtime_path(const LPCWSTR module_address,
    std::wstring& runtime_path) {
    HMODULE facade_module{};
    constexpr DWORD module_flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(module_flags, module_address, &facade_module) == FALSE) {
        return false;
    }

    std::wstring facade_path;
    if (!get_path_from_module(facade_module, facade_path) || !make_absolute_path(facade_path)) {
        return false;
    }

    const auto separator = facade_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return false;
    }
    facade_path.resize(separator + 1U);
    facade_path.append(L"inputnext_runtime.dll");
    runtime_path = with_extended_prefix(std::move(facade_path));
    return true;
}

template <typename Address>
[[nodiscard]] LPCWSTR as_module_address(const Address address) noexcept {
    return reinterpret_cast<LPCWSTR>(address);
}

template <std::size_t Count>
[[nodiscard]] inline bool callback_addresses_belong_to(const HMODULE provider,
    const std::array<LPCWSTR, Count>& addresses) noexcept {
    constexpr DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    for (const LPCWSTR address : addresses) {
        HMODULE address_module{};
        if (GetModuleHandleExW(flags, address, &address_module) == FALSE ||
            address_module != provider) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool pin_runtime_module(const HMODULE runtime_module,
    const LPCWSTR module_address) noexcept {
    HMODULE pinned_module{};
    constexpr DWORD pin_flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (GetModuleHandleExW(pin_flags, module_address, &pinned_module) == FALSE) {
        return false;
    }
    if (pinned_module != runtime_module) {
        static_cast<void>(FreeLibrary(pinned_module));
        return false;
    }
    return FreeLibrary(pinned_module) != FALSE;
}

} // namespace inputnext::facade_detail
