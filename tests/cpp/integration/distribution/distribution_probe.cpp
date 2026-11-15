#include "abi/pe_exports.hpp"
#include "abi_expected_exports.hpp"

#include <Windows.h>
#include <cstdio>
#include <dinput.h>
#include <exception>

#include <filesystem>
#include <iostream>
#include <span>

namespace {

using directinput8_create_fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using joystick_format_fn = LPCDIDATAFORMAT(WINAPI*)();

BOOL CALLBACK stop_after_device([[maybe_unused]] const LPCDIDEVICEINSTANCEA instance,
    [[maybe_unused]] const LPVOID context) {
    return DIENUM_STOP;
}

[[nodiscard]] bool check_exports(const std::filesystem::path& path,
    const std::span<const inputnext::test::abi::expected_export> exports) {
    const auto error = inputnext::test::abi::verify_pe_exports(path, exports);
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

[[nodiscard]] bool check_directinput(const std::filesystem::path& directory) {
    const auto facade_path = directory / L"dinput8.dll";
    HMODULE const facade = LoadLibraryExW(facade_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (facade == nullptr) {
        std::cerr << "DirectInput facade load failed: " << GetLastError() << '\n';
        return false;
    }

    const auto create =
        reinterpret_cast<directinput8_create_fn>(GetProcAddress(facade, "DirectInput8Create"));
    const auto get_format =
        reinterpret_cast<joystick_format_fn>(GetProcAddress(facade, "GetdfDIJoystick"));
    if (create == nullptr || get_format == nullptr || get_format() == nullptr) {
        std::cerr << "DirectInput facade exports are unusable\n";
        static_cast<void>(FreeLibrary(facade));
        return false;
    }

    IDirectInput8A* input{};
    const HRESULT create_status = create(GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8A,
        reinterpret_cast<LPVOID*>(&input),
        nullptr);
    if (create_status != DI_OK || input == nullptr) {
        std::cerr << "DirectInput8Create failed: 0x" << std::hex << create_status << '\n';
        static_cast<void>(FreeLibrary(facade));
        return false;
    }

    IDirectInput8W* wide{};
    const HRESULT query_status =
        input->QueryInterface(IID_IDirectInput8W, reinterpret_cast<void**>(&wide));
    if (query_status != S_OK || wide == nullptr) {
        std::cerr << "DirectInput A/W QueryInterface failed: 0x" << std::hex << query_status
                  << '\n';
        input->Release();
        static_cast<void>(FreeLibrary(facade));
        return false;
    }
    wide->Release();

    const bool facade_unloaded = FreeLibrary(facade) != FALSE;
    if (!facade_unloaded) {
        std::cerr << "DirectInput facade unload failed: " << GetLastError() << '\n';
        input->Release();
        return false;
    }

    const HRESULT keyboard_status =
        input->EnumDevices(DI8DEVCLASS_KEYBOARD, &stop_after_device, nullptr, DIEDFL_ATTACHEDONLY);
    const HRESULT mouse_status =
        input->EnumDevices(DI8DEVCLASS_POINTER, &stop_after_device, nullptr, DIEDFL_ATTACHEDONLY);
    input->Release();
    if (keyboard_status != DI_OK || mouse_status != DI_OK) {
        std::cerr << "DirectInput keyboard/mouse passthrough failed\n";
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
    using namespace inputnext::test::abi;
    const std::filesystem::path directory{argv[1]};
    if (!check_exports(directory / L"inputnext_runtime.dll", runtime_exports) ||
        !check_exports(directory / L"xinput1_3.dll", xinput13_exports) ||
        !check_exports(directory / L"xinput1_4.dll", xinput14_exports) ||
        !check_exports(directory / L"xinput9_1_0.dll", xinput910_exports) ||
        !check_exports(directory / L"dinput8.dll", dinput8_exports) ||
        !check_directinput(directory) || !check_gameinput(directory)) {
        return 1;
    }
    std::cout << "Package exports, DirectInput loading, and architecture verified\n";
    return 0;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "distribution_probe_error=%s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("distribution_probe_error=unknown exception\n", stderr));
    return 1;
}
