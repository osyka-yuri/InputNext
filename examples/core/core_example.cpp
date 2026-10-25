#include "core_reader.h"

#include <cstdio>
#include <exception>
#include <input_next_core.h>

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

class runtime_module final {
public:
    explicit runtime_module(const std::filesystem::path& path)
        : value_(LoadLibraryExW(path.c_str(),
              nullptr,
              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {}

    ~runtime_module() {
        if (value_ != nullptr) {
            FreeLibrary(value_);
        }
    }

    runtime_module(const runtime_module&) = delete;
    runtime_module& operator=(const runtime_module&) = delete;
    runtime_module(runtime_module&&) = delete;
    runtime_module& operator=(runtime_module&&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return value_; }

private:
    HMODULE value_{};
};

[[nodiscard]] bool complete_table(const input_next_core_api_v1* api) noexcept {
    return api != nullptr && api->struct_size == sizeof(*api) &&
           api->abi_version == INPUT_NEXT_CORE_ABI_VERSION && api->get_runtime_info != nullptr &&
           api->enumerate_devices != nullptr && api->resolve_target != nullptr &&
           api->read_latest != nullptr && api->open_reader != nullptr &&
           api->read_events != nullptr && api->close_reader != nullptr;
}

} // namespace

int wmain(const int argc, wchar_t** argv) try {
    const bool read_once = argc == 3 && std::wstring_view{argv[2]} == L"--read-once";
    if ((argc != 2 && !read_once) || !std::filesystem::path{argv[1]}.is_absolute()) {
        std::cerr << "Usage: inputnext_core_example <absolute-runtime-path> [--read-once]\n";
        return 2;
    }
    const runtime_module module{argv[1]};
    if (module.get() == nullptr) {
        std::cerr << "Runtime load failed: " << GetLastError() << '\n';
        return 2;
    }
    const auto query = reinterpret_cast<pfn_input_next_runtime_query_core_api>(
        GetProcAddress(module.get(), "input_next_runtime_query_core_api"));
    const auto* const api = query != nullptr ? query(INPUT_NEXT_CORE_ABI_VERSION) : nullptr;
    if (!complete_table(api)) {
        std::cerr << "Core ABI1 table is unavailable\n";
        return 2;
    }
    std::cout << "Core ABI1 table available\n";
    if (!read_once) {
        return 0;
    }
    const auto status = inputnext_example_read_once(api);
    std::cout << "Read status: " << status << '\n';
    return status == INPUT_NEXT_STATUS_OK || status == INPUT_NEXT_STATUS_NO_DATA ? 0 : 1;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "Core example failed: %s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("Core example failed: unknown exception\n", stderr));
    return 1;
}
