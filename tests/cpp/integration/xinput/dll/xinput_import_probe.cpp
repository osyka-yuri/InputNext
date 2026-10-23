#include "native_prerequisites.hpp"

#include <Windows.h>
#include <Xinput.h>
#include <cstdio>
#include <exception>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using inputnext::xinput_dll_tests::decide_native_prerequisites;
using inputnext::xinput_dll_tests::native_prerequisite_state;

[[nodiscard]] std::filesystem::path module_path(const HMODULE module) {
    std::wstring buffer(512U, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32'768U));
    }
}

[[nodiscard]] bool path_matches(const HMODULE module, const std::filesystem::path& expected) {
    if (module == nullptr) {
        return false;
    }
    std::error_code error;
    return std::filesystem::equivalent(module_path(module), expected, error) && !error;
}

[[nodiscard]] std::filesystem::path executable_directory() {
    const auto current = module_path(nullptr);
    return current.empty() ? std::filesystem::path{} : current.parent_path();
}

[[nodiscard]] bool bundle_files_present(const std::filesystem::path& directory) {
    constexpr std::array<const wchar_t*, 6U> names{L"xinput1_3.dll",
        L"xinput1_4.dll",
        L"xinput9_1_0.dll",
        L"inputnext_runtime.dll",
        L"GameInputRedist.dll",
        L"GameInputRawInputProxy.exe"};
    std::error_code error;
    for (const auto* const name : names) {
        if (!std::filesystem::is_regular_file(directory / name, error) || error) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool production_bundle_matches(const std::filesystem::path& directory) {
    if (directory.empty() || !bundle_files_present(directory) ||
        !path_matches(GetModuleHandleW(L"xinput1_4.dll"), directory / L"xinput1_4.dll")) {
        return false;
    }
    return path_matches(GetModuleHandleW(L"inputnext_runtime.dll"),
        directory / L"inputnext_runtime.dll");
}

[[nodiscard]] bool is_valid_native_result(const DWORD result) noexcept {
    return result == ERROR_SUCCESS || result == ERROR_DEVICE_NOT_CONNECTED;
}

[[nodiscard]] int run_import_smoke() {
    const auto directory = executable_directory();
#ifdef INPUTNEXT_XINPUT_IMPORT13
    if (!path_matches(GetModuleHandleW(L"xinput1_3.dll"), directory / L"xinput1_3.dll") ||
        GetModuleHandleW(L"inputnext_runtime.dll") != nullptr) {
        std::cerr << "xinput_import13_startup=unexpected-module\n";
        return 1;
    }
    std::cout << "xinput_import13_startup=app-local-lazy\n";
#endif
    if (!path_matches(GetModuleHandleW(L"xinput1_4.dll"), directory / L"xinput1_4.dll")) {
        std::cerr << "xinput_import_route=not-app-local\n";
        return 1;
    }
    std::cout << "xinput_import_route=app-local\n";

    XINPUT_STATE state{};
    std::memset(&state, 0x4D, sizeof(state));
    XINPUT_STATE before{};
    std::memcpy(&before, &state, sizeof(before));
    const DWORD result = XInputGetState(0U, &state);
    if (!is_valid_native_result(result)) {
        std::cerr << "xinput_import_call=unexpected-result\n";
        return 1;
    }
    if (result == ERROR_DEVICE_NOT_CONNECTED && std::memcmp(&state, &before, sizeof(state)) != 0) {
        std::cerr << "xinput_import_call=changed-error-output\n";
        return 1;
    }
    if (!path_matches(GetModuleHandleW(L"inputnext_runtime.dll"),
            directory / L"inputnext_runtime.dll")) {
        std::cerr << "xinput_import_runtime=not-app-local\n";
        return 1;
    }

    std::cout << "xinput_import_call=" << (result == ERROR_SUCCESS ? "success" : "disconnected")
              << '\n';
    return 0;
}

[[nodiscard]] LRESULT CALLBACK native_window_proc(const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] HWND create_manual_window() {
    constexpr auto class_name = std::to_array(L"InputNextXInputNativeManualProbe");
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &native_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name.data();
    if (RegisterClassW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    HWND const window = CreateWindowExW(WS_EX_APPWINDOW,
        class_name.data(),
        L"InputNext XInput native check - activate this window manually",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        180,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    if (window == nullptr) {
        return nullptr;
    }
    static_cast<void>(ShowWindow(window, SW_SHOWNOACTIVATE));
    static_cast<void>(UpdateWindow(window));
    return window;
}

[[nodiscard]] const char* state_name(const native_prerequisite_state state) noexcept {
    switch (state) {
    case native_prerequisite_state::foreground_missing:
        return "foreground-missing";
    case native_prerequisite_state::production_bundle_mismatch:
        return "bundle-mismatch";
    case native_prerequisite_state::controller_disconnected:
        return "controller-disconnected";
    case native_prerequisite_state::ready:
        return "ready";
    }
    return "unknown";
}

[[nodiscard]] int run_native_manual() {
    HWND const window = create_manual_window();
    if (window == nullptr) {
        std::cerr << "manual_native_window=creation-failed\n";
        return 1;
    }

    std::cout << "manual_native_window=shown-without-activation\n"
              << "Activate the InputNext XInput native check window during the 120-second "
                 "observation window.\n"
              << "A synchronous native XInput call can delay the next check and completion.\n"
              << "A connected controller in slot 0 is required.\n"
              << std::flush;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    auto last_state = native_prerequisite_state::foreground_missing;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) {
                std::cout << "manual_native_prerequisites=" << state_name(last_state) << '\n';
                return 1;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        const bool foreground = GetForegroundWindow() == window;
        const auto directory = executable_directory();
        bool bundle_matches =
            !directory.empty() && bundle_files_present(directory) &&
            path_matches(GetModuleHandleW(L"xinput1_4.dll"), directory / L"xinput1_4.dll");
        bool connected{};
        if (foreground && bundle_matches) {
            XINPUT_STATE state{};
            connected = XInputGetState(0U, &state) == ERROR_SUCCESS;
            bundle_matches = production_bundle_matches(directory);
        }
        last_state = decide_native_prerequisites({.foreground = foreground,
            .production_bundle_matches = bundle_matches,
            .controller_connected = connected});
        if (last_state == native_prerequisite_state::ready) {
            std::cout << "manual_native_prerequisites=ready\n";
            static_cast<void>(DestroyWindow(window));
            return 0;
        }
        Sleep(50U);
    }

    std::cout << "manual_native_prerequisites=" << state_name(last_state) << '\n';
    static_cast<void>(DestroyWindow(window));
    return 1;
}

} // namespace

int wmain(const int argc, wchar_t** const argv) try {
    if (argc == 1) {
        return run_import_smoke();
    }
    const std::wstring_view mode{argv[1]};
    if (mode == L"--native-manual" && argc == 2) {
        return run_native_manual();
    }
    std::cerr << "xinput_import_probe_usage=executable [--native-manual]\n";
    return 2;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "xinput_import_probe_error=%s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("xinput_import_probe_error=unknown exception\n", stderr));
    return 1;
}
