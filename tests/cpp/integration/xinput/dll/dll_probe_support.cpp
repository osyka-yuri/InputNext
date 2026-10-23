#include "dll_probe_support.hpp"

#include "table_fixture_protocol.hpp"

#include <TlHelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace inputnext::xinput_dll_probe {

constexpr DWORD facade_search_flags =
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] std::wstring extended_path(std::wstring path) {
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) {
        return path;
    }
    if (path.starts_with(L"\\\\")) {
        return std::wstring{L"\\\\?\\UNC\\"} + path.substr(2U);
    }
    return std::wstring{L"\\\\?\\"} + path;
}

[[nodiscard]] module_handle load_library(const std::filesystem::path& path) {
    const auto full_path = std::filesystem::absolute(path).wstring();
    const auto extended = extended_path(full_path);
    return module_handle{LoadLibraryExW(extended.c_str(), nullptr, facade_search_flags)};
}

[[nodiscard]] DWORD
call_get_state(const HMODULE facade, const DWORD user_index, XINPUT_STATE* const state) {
    const auto get_state = load_named_function<get_state_fn>(facade, "XInputGetState");
    return get_state(user_index, state);
}

[[nodiscard]] std::wstring module_filename(const HMODULE module) {
    std::wstring buffer(512U, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        require(length != 0U, "GetModuleFileNameW failed");
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        require(buffer.size() < 32'768U, "module path exceeded the Windows path limit");
        buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32'768U));
    }
}

[[nodiscard]] bool module_path_matches(const HMODULE module,
    const std::filesystem::path& expected) {
    std::error_code error;
    return std::filesystem::equivalent(std::filesystem::path{module_filename(module)},
               expected,
               error) &&
           !error;
}

[[nodiscard]] bool state_is_unchanged(const XINPUT_STATE& actual,
    const XINPUT_STATE& before) noexcept {
    return std::memcmp(&actual, &before, sizeof(actual)) == 0;
}

[[nodiscard]] DWORD call_valid_state_and_check_output(const HMODULE facade) {
    XINPUT_STATE state{};
    std::memset(&state, 0x5A, sizeof(state));
    XINPUT_STATE before{};
    std::memcpy(&before, &state, sizeof(before));
    const DWORD result = call_get_state(facade, 0U, &state);
    require(result == ERROR_SUCCESS || result == ERROR_DEVICE_NOT_CONNECTED,
        "GetState returned an unexpected valid-index result");
    if (result != ERROR_SUCCESS) {
        require(state_is_unchanged(state, before), "GetState changed output after an error");
    }
    return result;
}

[[nodiscard]] DWORD call_invalid_state_and_check_output(const HMODULE facade) {
    XINPUT_STATE state{};
    std::memset(&state, 0x5A, sizeof(state));
    XINPUT_STATE before{};
    std::memcpy(&before, &state, sizeof(before));
    const DWORD result = call_get_state(facade, XUSER_MAX_COUNT, &state);
    require(state_is_unchanged(state, before), "GetState changed output for an invalid index");
    require(result == ERROR_BAD_ARGUMENTS, "GetState did not reject an invalid index exactly");
    return result;
}

[[nodiscard]] DWORD module_not_loaded() noexcept {
    return GetModuleHandleW(L"inputnext_runtime.dll") == nullptr ? 1U : 0U;
}

[[nodiscard]] DWORD runtime_module_count() {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    DWORD snapshot_error{};
    constexpr std::uint32_t snapshot_attempts = 8U;
    for (std::uint32_t attempt = 0U; attempt < snapshot_attempts; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            break;
        }
        snapshot_error = GetLastError();
        if (snapshot_error != ERROR_BAD_LENGTH) {
            break;
        }
    }
    require(snapshot != INVALID_HANDLE_VALUE,
        "module snapshot failed: " + std::to_string(snapshot_error));
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD count{};
    if (Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(entry.szModule, L"inputnext_runtime.dll") == 0) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (Module32NextW(snapshot, &entry) != FALSE);
    }
    const DWORD enumeration_error = GetLastError();
    require(CloseHandle(snapshot) != FALSE, "could not release the module snapshot");
    require(enumeration_error == ERROR_NO_MORE_FILES,
        "module enumeration failed: " + std::to_string(enumeration_error));
    return count;
}

[[nodiscard]] DWORD ensure_query_count(const snapshot_fn get_snapshot,
    const std::uint32_t expected_count) {
    snapshot state{};
    require(get_snapshot(&state) != FALSE, "fixture snapshot failed");
    require(state.query_calls == expected_count, "fixture query count did not match");
    return state.last_requested_abi;
}

void expect_last_callback(const snapshot_fn get_snapshot,
    const callback_id callback,
    const std::uint32_t flavor,
    const std::initializer_list<std::uintptr_t> expected_arguments) {
    snapshot state{};
    require(get_snapshot(&state) != FALSE, "fixture snapshot failed");
    require(state.last_callback == static_cast<std::uint32_t>(callback),
        "facade selected the wrong ABI callback");
    require(state.last_flavor == flavor, "facade selected the wrong fixed flavor");
    require(state.last_callback_result == expected_result(callback),
        "fixture callback result did not match its callback");
    std::size_t index{};
    for (const auto argument : expected_arguments) {
        require(index < std::size(state.last_arguments), "too many expected callback arguments");
        require(state.last_arguments[index] == argument, "facade changed a callback argument");
        ++index;
    }
    while (index < std::size(state.last_arguments)) {
        require(state.last_arguments[index] == 0U,
            "facade supplied an unexpected callback argument");
        ++index;
    }
}

[[nodiscard]] bool
configure_fixture(const HMODULE module, const query_mode mode, const HMODULE reentry_target) {
    const auto configure = load_named_function<configure_fn>(module, "inputnext_fixture_configure");
    return configure(static_cast<std::uint32_t>(mode), reentry_target) != FALSE;
}

void reset_fixture(const HMODULE module) {
    const auto reset = load_named_function<reset_fn>(module, "inputnext_fixture_reset");
    reset();
}

void expect_reentry_result(const snapshot_fn get_snapshot,
    const std::uint32_t expected_queries,
    const callback_id final_callback) {
    snapshot state{};
    require(get_snapshot(&state) != FALSE, "fixture snapshot failed");
    require(state.query_calls == expected_queries, "reentrant query count did not match");
    require(state.reentry_calls == 1U, "fixture did not perform one reentrant call");
    require(state.reentry_result == ERROR_DEVICE_NOT_CONNECTED,
        "reentrant facade call did not fail closed");
    require(state.callback_calls == 1U &&
                state.last_callback == static_cast<std::uint32_t>(final_callback),
        "outer facade call did not reach its callback exactly once");
}

} // namespace inputnext::xinput_dll_probe
