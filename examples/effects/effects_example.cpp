#include "effects_demo.h"

#include <cstdio>
#include <exception>
#include <input_next_core.h>
#include <input_next_effects.h>

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

[[nodiscard]] bool complete_table(const input_next_effects_api_v1* api) noexcept {
    return api != nullptr && api->struct_size == sizeof(*api) &&
           api->abi_version == INPUT_NEXT_EFFECTS_ABI_VERSION && api->resolve_target != nullptr &&
           api->open_context != nullptr && api->query_context != nullptr &&
           api->submit != nullptr && api->clear != nullptr && api->submit_body != nullptr &&
           api->clear_body != nullptr && api->close_context != nullptr &&
           api->ack_close != nullptr && api->query_progress != nullptr;
}

} // namespace

int wmain(const int argc, wchar_t** argv) try {
    const bool rumble = argc == 3 && std::wstring_view{argv[2]} == L"--rumble";
    const bool body = argc == 3 && std::wstring_view{argv[2]} == L"--body";
    if ((argc != 2 && !rumble && !body) || !std::filesystem::path{argv[1]}.is_absolute()) {
        std::cerr << "Usage: inputnext_effects_example <absolute-runtime-path> [--rumble|--body]\n";
        return 2;
    }
    const runtime_module module{argv[1]};
    if (module.get() == nullptr) {
        std::cerr << "Runtime load failed: " << GetLastError() << '\n';
        return 2;
    }
    const auto query = reinterpret_cast<pfn_input_next_runtime_query_effects_api>(
        GetProcAddress(module.get(), "input_next_runtime_query_effects_api"));
    const auto* const api = query != nullptr ? query(INPUT_NEXT_EFFECTS_ABI_VERSION) : nullptr;
    if (!complete_table(api)) {
        std::cerr << "Effects ABI1 table is unavailable\n";
        return 2;
    }
    std::cout << "Effects ABI1 table available\n";
    if (!rumble && !body) {
        return 0;
    }
    std::cout << "Keep this window in the foreground. Press Enter to send a short effect to "
                 "controller position 0.\n";
    if (std::cin.get() != '\n') {
        return 1;
    }
    const auto status = inputnext_example_effect(api, body ? 1 : 0);
    std::cout << "Result: " << status << '\n';
    return status == INPUT_NEXT_STATUS_OK ? 0 : 1;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "Effects example failed: %s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("Effects example failed: unknown exception\n", stderr));
    return 1;
}
