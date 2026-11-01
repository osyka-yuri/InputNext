#pragma once

#include "fixtures/table_fixture_protocol.hpp"
#include "runtime/abi/xinput/api.hpp"

#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace inputnext::xinput_dll_probe {

using namespace inputnext::xinput_dll_fixture;

void require(bool condition, std::string_view message);

class module_handle final {
public:
    explicit module_handle(HMODULE value = nullptr) noexcept : value_{value} {}

    ~module_handle() { static_cast<void>(close()); }

    module_handle(const module_handle&) = delete;
    module_handle& operator=(const module_handle&) = delete;

    module_handle(module_handle&& other) noexcept : value_{std::exchange(other.value_, nullptr)} {}

    module_handle& operator=(module_handle&& other) noexcept {
        if (this != &other) {
            if (!close()) {
                return *this;
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HMODULE get() const noexcept { return value_; }

    [[nodiscard]] bool close() noexcept {
        if (value_ == nullptr) {
            return true;
        }
        if (FreeLibrary(value_) == FALSE) {
            return false;
        }
        value_ = nullptr;
        return true;
    }

private:
    HMODULE value_{};
};

template <typename Function>
[[nodiscard]] Function load_named_function(const HMODULE module, const char* const name) {
    const FARPROC address = GetProcAddress(module, name);
    require(address != nullptr, std::string{"missing named export: "} + name);
    const auto function = reinterpret_cast<Function>(address);
    require(function != nullptr, std::string{"invalid named export: "} + name);
    return function;
}

template <typename Function>
[[nodiscard]] Function load_ordinal_function(const HMODULE module, const WORD ordinal) {
    const FARPROC address = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    require(address != nullptr, "missing ordinal export");
    const auto function = reinterpret_cast<Function>(address);
    require(function != nullptr, "invalid ordinal export");
    return function;
}

using get_state_fn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept;
using api_table_fn = const input_next_runtime_api_v1*(__cdecl*)() noexcept;

struct missing_query_retry_request final {
    std::filesystem::path directory;
    std::filesystem::path valid_fixture;
    std::filesystem::path missing_query_fixture;
};

struct ar1_rejection_request final {
    std::filesystem::path directory;
    std::filesystem::path valid_fixture;
};

[[nodiscard]] std::wstring extended_path(std::wstring path);
[[nodiscard]] module_handle load_library(const std::filesystem::path& path);
[[nodiscard]] DWORD call_get_state(HMODULE facade, DWORD user_index, XINPUT_STATE* state);
[[nodiscard]] std::wstring module_filename(HMODULE module);
[[nodiscard]] bool module_path_matches(HMODULE module, const std::filesystem::path& expected);
[[nodiscard]] HMODULE imported_companion(HMODULE facade);
[[nodiscard]] bool state_is_unchanged(const XINPUT_STATE& actual,
    const XINPUT_STATE& before) noexcept;
[[nodiscard]] DWORD call_valid_state_and_check_output(HMODULE facade);
[[nodiscard]] DWORD call_invalid_state_and_check_output(HMODULE facade);
[[nodiscard]] DWORD module_not_loaded() noexcept;
[[nodiscard]] DWORD runtime_module_count();
[[nodiscard]] DWORD ensure_query_count(snapshot_fn get_snapshot, std::uint32_t expected_count);
void expect_last_callback(snapshot_fn get_snapshot,
    callback_id callback,
    std::uint32_t flavor,
    std::initializer_list<std::uintptr_t> expected_arguments);
[[nodiscard]] bool
configure_fixture(HMODULE module, query_mode mode, HMODULE reentry_target = nullptr);
void reset_fixture(HMODULE module);
void expect_reentry_result(snapshot_fn get_snapshot,
    std::uint32_t expected_queries,
    callback_id final_callback);

[[nodiscard]] int run_production(const std::filesystem::path& bundle);
[[nodiscard]] int run_companion(const std::filesystem::path& bundle);
[[nodiscard]] int run_companion_preloaded(const std::filesystem::path& bundle);
[[nodiscard]] int run_companion_missing(const std::filesystem::path& directory);
[[nodiscard]] int run_companion_system_path(const std::filesystem::path& bundle);
[[nodiscard]] int run_core_query(const std::filesystem::path& bundle);
[[nodiscard]] int run_provider_lifetime(const std::filesystem::path& directory);
[[nodiscard]] int run_external_addresses(const std::filesystem::path& directory);
[[nodiscard]] int run_table_dispatch(const std::filesystem::path& directory);
[[nodiscard]] int run_malformed_retry(const std::filesystem::path& directory,
    const std::array<std::filesystem::path, 4U>& malformed_fixtures,
    const std::filesystem::path& valid_fixture);
[[nodiscard]] int run_missing_query_retry(const missing_query_retry_request& request);
[[nodiscard]] int run_reentry(const std::filesystem::path& directory, bool cross_facade);
[[nodiscard]] int run_ar1_rejection(const ar1_rejection_request& request);

} // namespace inputnext::xinput_dll_probe
