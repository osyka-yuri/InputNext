#include "system_input.hpp"

#include "native_types.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace inputnext::dinput {
namespace {

using direct_input8_create_fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
using joystick_format_fn = LPCDIDATAFORMAT(WINAPI*)();

struct release_module final {
    void operator()(HMODULE module) const noexcept { static_cast<void>(FreeLibrary(module)); }
};

using module_owner = std::unique_ptr<std::remove_pointer_t<HMODULE>, release_module>;

struct provider final {
    std::mutex gate;
    module_owner module;
    direct_input8_create_fn create{};
    joystick_format_fn joystick_format{};
    HRESULT status{E_UNEXPECTED};
    bool attempted{};
};

[[nodiscard]] provider* process_provider() noexcept {
    static auto* const value = new (std::nothrow) provider{};
    return value;
}

[[nodiscard]] HRESULT win32_error(const DWORD error) noexcept {
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : error);
}

[[nodiscard]] UINT get_architecture_system_directory(LPWSTR path, const UINT capacity) noexcept {
#if defined(_M_IX86) || defined(__i386__)
    BOOL under_wow64{};
    if (IsWow64Process(GetCurrentProcess(), &under_wow64) == FALSE) {
        return 0U;
    }
    if (under_wow64 != FALSE) {
        return GetSystemWow64DirectoryW(path, capacity);
    }
#endif
    return GetSystemDirectoryW(path, capacity);
}

[[nodiscard]] std::expected<HMODULE, HRESULT> load_system_module(std::wstring& expected_path) {
    constexpr std::size_t max_system_directory = 32'767U;
    std::vector<wchar_t> directory(512U, L'\0');
    UINT written{};
    for (;;) {
        written = get_architecture_system_directory(directory.data(),
            static_cast<UINT>(directory.size()));
        if (written == 0U) {
            return std::unexpected{win32_error(GetLastError())};
        }
        if (static_cast<std::size_t>(written) < directory.size()) {
            break;
        }
        if (static_cast<std::size_t>(written) >= max_system_directory) {
            return std::unexpected{HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE)};
        }
        directory.resize(static_cast<std::size_t>(written) + 1U, L'\0');
    }

    expected_path.assign(directory.data(), written);
    if (!expected_path.empty() && expected_path.back() != L'\\' && expected_path.back() != L'/') {
        expected_path.push_back(L'\\');
    }
    expected_path.append(L"dinput8.dll");

    auto* const module =
        LoadLibraryExW(expected_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        return std::unexpected{win32_error(GetLastError())};
    }
    return module;
}

[[nodiscard]] std::optional<FILE_ID_INFO> file_identity(const wchar_t* path) noexcept {
    auto* const file = CreateFileW(path,
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_ID_INFO identity{};
    const auto queried =
        GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity));
    const auto closed = CloseHandle(file);
    if (queried == FALSE || closed == FALSE) {
        return std::nullopt;
    }
    return identity;
}

[[nodiscard]] bool same_module_path(const HMODULE module, const std::wstring& expected_path) {
    std::vector<wchar_t> actual(512U, L'\0');
    for (;;) {
        const auto length =
            GetModuleFileNameW(module, actual.data(), static_cast<DWORD>(actual.size()));
        if (length == 0U) {
            return false;
        }
        if (static_cast<std::size_t>(length) < actual.size() - 1U) {
            actual.resize(static_cast<std::size_t>(length));
            break;
        }
        if (actual.size() >= 32'768U) {
            return false;
        }
        actual.resize(std::min(actual.size() * 2U, static_cast<std::size_t>(32'768U)), L'\0');
    }

    if (CompareStringOrdinal(actual.data(),
            static_cast<int>(actual.size()),
            expected_path.data(),
            static_cast<int>(expected_path.size()),
            TRUE) == CSTR_EQUAL) {
        return true;
    }

    // WOW64 can report a System32 alias after an absolute SysWOW64 load.
    // Accept the alias only when both paths identify the same physical file.
    actual.push_back(L'\0');
    const auto loaded_identity = file_identity(actual.data());
    const auto expected_identity = file_identity(expected_path.c_str());
    return loaded_identity.has_value() && expected_identity.has_value() &&
           loaded_identity->VolumeSerialNumber == expected_identity->VolumeSerialNumber &&
           std::memcmp(&loaded_identity->FileId, &expected_identity->FileId, sizeof(FILE_ID_128)) ==
               0;
}

[[nodiscard]] HRESULT ensure_provider(provider& value) noexcept {
    std::scoped_lock lock{value.gate};
    if (value.attempted) {
        return value.status;
    }
    value.attempted = true;

    try {
        std::wstring expected_path;
        const auto loaded = load_system_module(expected_path);
        if (!loaded.has_value()) {
            value.status = loaded.error();
            return value.status;
        }
        module_owner module{*loaded};
        if (!same_module_path(module.get(), expected_path)) {
            value.status = HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
            return value.status;
        }

        const auto create_proc = GetProcAddress(module.get(), MAKEINTRESOURCEA(1U));
        const auto format_proc = GetProcAddress(module.get(), MAKEINTRESOURCEA(6U));
        if (create_proc == nullptr || format_proc == nullptr) {
            value.status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            return value.status;
        }

        static_assert(sizeof(value.create) == sizeof(create_proc));
        static_assert(sizeof(value.joystick_format) == sizeof(format_proc));
        value.create = reinterpret_cast<direct_input8_create_fn>(create_proc);
        value.joystick_format = reinterpret_cast<joystick_format_fn>(format_proc);
        value.module = std::move(module);
        value.status = S_OK;
        return S_OK;
    } catch (const std::bad_alloc&) {
        value.status = E_OUTOFMEMORY;
        return value.status;
    } catch (...) {
        value.status = E_FAIL;
        return value.status;
    }
}

} // namespace

HRESULT create_system_input([[maybe_unused]] void* context,
    HINSTANCE instance,
    DWORD version,
    REFIID iid,
    void** out,
    IUnknown* const outer) noexcept {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;

    auto* state = process_provider();
    if (state == nullptr) {
        return E_OUTOFMEMORY;
    }
    const auto status = ensure_provider(*state);
    if (FAILED(status)) {
        return status;
    }

    return state->create(instance, version, iid, out, outer);
}

LPCDIDATAFORMAT system_joystick_format() noexcept {
    auto* state = process_provider();
    if (state == nullptr || FAILED(ensure_provider(*state))) {
        return nullptr;
    }
    return state->joystick_format();
}

} // namespace inputnext::dinput
