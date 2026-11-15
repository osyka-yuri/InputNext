#include "abi/pe_exports.hpp"
#include "abi_expected_exports.hpp"
#include "runtime/abi/dinput/api.hpp"

#include <Windows.h>
#include <dinput.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
#    include "abi/ubsan_runtime_exports.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using directinput8_create_fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using can_unload_fn = HRESULT(WINAPI*)();
using get_class_object_fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using simple_export_fn = HRESULT(WINAPI*)();
using joystick_format_fn = LPCDIDATAFORMAT(WINAPI*)();

[[nodiscard]] HMODULE load_facade() {
    const auto path = std::filesystem::path{INPUTNEXT_DINPUT_PROBE_DIRECTORY} / L"dinput8.dll";
    return LoadLibraryExW(path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

template <typename Function>
[[nodiscard]] Function ordinal_export(const HMODULE module, const WORD ordinal) {
    return reinterpret_cast<Function>(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
}

[[nodiscard]] std::string verify_exports(const std::filesystem::path& path,
    const std::span<const inputnext::test::abi::expected_export> expected) {
#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
    return inputnext::test::abi::verify_pe_exports(path,
        expected,
        inputnext::test::abi::ubsan_runtime_export_names);
#else
    return inputnext::test::abi::verify_pe_exports(path, expected);
#endif
}

struct enumeration_result final {
    enumeration_result() : instance_ids(std::allocator<GUID>{}) {}

    std::vector<GUID> instance_ids;
    bool allocation_failed{};
};

BOOL CALLBACK collect_native_device_a(const LPCDIDEVICEINSTANCEA instance,
    const LPVOID context) noexcept {
    auto* const result = static_cast<enumeration_result*>(context);
    if (instance != nullptr && result != nullptr) {
        try {
            result->instance_ids.push_back(instance->guidInstance);
        } catch (...) {
            result->allocation_failed = true;
            return DIENUM_STOP;
        }
    }
    return DIENUM_CONTINUE;
}

[[nodiscard]] bool enumerate_instance_ids(IDirectInput8A* const input,
    const DWORD device_class,
    enumeration_result& result) {
    return input != nullptr &&
           input->EnumDevices(device_class,
               &collect_native_device_a,
               &result,
               DIEDFL_ATTACHEDONLY) == DI_OK &&
           !result.allocation_failed;
}

[[nodiscard]] bool same_instance_ids(const enumeration_result& left,
    const enumeration_result& right) {
    return left.instance_ids.size() == right.instance_ids.size() &&
           std::ranges::all_of(left.instance_ids, [&right](const GUID& instance_id) {
               const auto matching =
                   std::ranges::count_if(right.instance_ids, [&instance_id](const GUID& candidate) {
                       return IsEqualGUID(instance_id, candidate) != FALSE;
                   });
               return matching == 1;
           });
}

} // namespace

TEST_CASE("dinput.dll.exports_match_system_ordinals_and_defer_registry_changes",
    "[integration][dinput][dll][abi]") {
    const auto facade_path =
        std::filesystem::path{INPUTNEXT_DINPUT_PROBE_DIRECTORY} / L"dinput8.dll";
    const auto export_error = verify_exports(facade_path, inputnext::test::abi::dinput8_exports);
    INFO(export_error);
    REQUIRE(export_error.empty());

    HMODULE const module = load_facade();
    REQUIRE(module != nullptr);
    CHECK(GetModuleHandleW(L"inputnext_runtime.dll") == nullptr);

    constexpr std::array<const char*, 6U> names{"DirectInput8Create",
        "DllCanUnloadNow",
        "DllGetClassObject",
        "DllRegisterServer",
        "DllUnregisterServer",
        "GetdfDIJoystick"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const WORD ordinal = static_cast<WORD>(index + 1U);
        CHECK(GetProcAddress(module, names[index]) != nullptr);
        CHECK(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)) != nullptr);
    }

    const auto register_server = ordinal_export<simple_export_fn>(module, 4U);
    const auto unregister_server = ordinal_export<simple_export_fn>(module, 5U);
    REQUIRE(register_server != nullptr);
    REQUIRE(unregister_server != nullptr);
    CHECK(register_server() == E_NOTIMPL);
    CHECK(unregister_server() == E_NOTIMPL);
    CHECK(GetModuleHandleW(L"inputnext_runtime.dll") == nullptr);

    const auto get_format = ordinal_export<joystick_format_fn>(module, 6U);
    REQUIRE(get_format != nullptr);
    const LPCDIDATAFORMAT format = get_format();
    REQUIRE(format != nullptr);
    CHECK(format->dwSize == sizeof(DIDATAFORMAT));
    CHECK(format->dwObjSize == sizeof(DIOBJECTDATAFORMAT));
    CHECK(format->dwNumObjs > 0U);
    CHECK(format->dwDataSize >= sizeof(DIJOYSTATE));

    CHECK(FreeLibrary(module) != FALSE);
}

TEST_CASE("dinput.dll.runtime_private_table_has_the_frozen_shape",
    "[integration][dinput][dll][abi]") {
    const auto runtime_path =
        std::filesystem::path{INPUTNEXT_DINPUT_PROBE_DIRECTORY} / L"inputnext_runtime.dll";
    const auto export_error = verify_exports(runtime_path, inputnext::test::abi::runtime_exports);
    INFO(export_error);
    REQUIRE(export_error.empty());

    HMODULE const runtime = LoadLibraryExW(runtime_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (runtime == nullptr) {
        FAIL("Failed to load the InputNext runtime DLL");
        return;
    }

    const FARPROC query_address =
        GetProcAddress(runtime, "input_next_runtime_query_directinput_api");
    REQUIRE(query_address != nullptr);
    const auto query = reinterpret_cast<input_next_query_directinput_api_fn>(query_address);
    const auto* const api = query(1U);
    REQUIRE(is_valid_input_next_directinput_api_v1(api));

    CHECK(query(0U) == nullptr);
    HMODULE owner{};
    constexpr DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    const std::array<LPCWSTR, 4U> callback_addresses{reinterpret_cast<LPCWSTR>(api->create),
        reinterpret_cast<LPCWSTR>(api->get_class_object),
        reinterpret_cast<LPCWSTR>(api->can_unload),
        reinterpret_cast<LPCWSTR>(api->joystick_format)};
    for (const LPCWSTR address : callback_addresses) {
        owner = nullptr;
        CHECK(GetModuleHandleExW(flags, address, &owner) != FALSE);
        CHECK(owner == runtime);
    }

    CHECK(FreeLibrary(runtime) != FALSE);
}

TEST_CASE("dinput.dll_can_unload_tracks_factories_server_locks_and_root_objects",
    "[integration][dinput][dll][com]") {
    HMODULE const module = load_facade();
    REQUIRE(module != nullptr);
    const auto create = ordinal_export<directinput8_create_fn>(module, 1U);
    const auto can_unload = ordinal_export<can_unload_fn>(module, 2U);
    const auto get_class = ordinal_export<get_class_object_fn>(module, 3U);
    REQUIRE(create != nullptr);
    REQUIRE(can_unload != nullptr);
    REQUIRE(get_class != nullptr);
    CHECK(can_unload() == S_OK);

    IClassFactory* factory{};
    REQUIRE(get_class(CLSID_DirectInput8, IID_IClassFactory, reinterpret_cast<LPVOID*>(&factory)) ==
            S_OK);
    REQUIRE(factory != nullptr);
    CHECK(can_unload() == S_FALSE);
    CHECK(factory->LockServer(TRUE) == S_OK);
    CHECK(can_unload() == S_FALSE);
    CHECK(factory->LockServer(FALSE) == S_OK);
    CHECK(factory->Release() == 0U);
    CHECK(can_unload() == S_OK);

    IDirectInput8A* input{};
    REQUIRE(create(GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                reinterpret_cast<LPVOID*>(&input),
                nullptr) == DI_OK);
    REQUIRE(input != nullptr);
    CHECK(can_unload() == S_FALSE);
    CHECK(input->Release() == 0U);
    CHECK(can_unload() == S_OK);
    CHECK(FreeLibrary(module) != FALSE);
}

TEST_CASE("dinput.dll_com_identity_factory_and_runtime_lifetime_are_coherent",
    "[integration][dinput][dll][com]") {
    HMODULE const module = load_facade();
    REQUIRE(module != nullptr);

    const auto create = ordinal_export<directinput8_create_fn>(module, 1U);
    const auto can_unload = ordinal_export<can_unload_fn>(module, 2U);
    const auto get_class = ordinal_export<get_class_object_fn>(module, 3U);
    REQUIRE(create != nullptr);
    REQUIRE(can_unload != nullptr);
    REQUIRE(get_class != nullptr);

    IClassFactory* factory{};
    REQUIRE(get_class(CLSID_DirectInput8, IID_IClassFactory, reinterpret_cast<LPVOID*>(&factory)) ==
            S_OK);
    REQUIRE(factory != nullptr);

    LPVOID object{};
    CHECK(create(GetModuleHandleW(nullptr),
              DIRECTINPUT_VERSION,
              IID_IDirectInput8A,
              &object,
              factory) == CLASS_E_NOAGGREGATION);
    CHECK(object == nullptr);

    CHECK(can_unload() == S_FALSE);
    CHECK(factory->LockServer(TRUE) == S_OK);
    CHECK(can_unload() == S_FALSE);
    CHECK(factory->LockServer(FALSE) == S_OK);

    LPVOID aggregated{};
    CHECK(
        factory->CreateInstance(factory, IID_IDirectInput8A, &aggregated) == CLASS_E_NOAGGREGATION);
    CHECK(aggregated == nullptr);

    IDirectInput8A* uninitialized{};
    REQUIRE(factory->CreateInstance(nullptr,
                IID_IDirectInput8A,
                reinterpret_cast<LPVOID*>(&uninitialized)) == S_OK);
    REQUIRE(uninitialized != nullptr);
    CHECK(uninitialized->Initialize(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION) == DI_OK);

    IDirectInput8W* wide{};
    REQUIRE(
        uninitialized->QueryInterface(IID_IDirectInput8W, reinterpret_cast<void**>(&wide)) == S_OK);
    REQUIRE(wide != nullptr);
    IUnknown* narrow_identity{};
    IUnknown* wide_identity{};
    REQUIRE(uninitialized->QueryInterface(IID_IUnknown,
                reinterpret_cast<void**>(&narrow_identity)) == S_OK);
    REQUIRE(wide->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&wide_identity)) == S_OK);
    CHECK(narrow_identity == wide_identity);

    IDirectInput7A* old_interface{};
    CHECK(uninitialized->QueryInterface(IID_IDirectInput7A,
              reinterpret_cast<void**>(&old_interface)) == E_NOINTERFACE);
    CHECK(old_interface == nullptr);

    IDirectInput8A* created_ansi{};
    IDirectInput8W* created_wide{};
    REQUIRE(create(GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8A,
                reinterpret_cast<LPVOID*>(&created_ansi),
                nullptr) == DI_OK);
    REQUIRE(create(GetModuleHandleW(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8W,
                reinterpret_cast<LPVOID*>(&created_wide),
                nullptr) == DI_OK);
    REQUIRE(created_ansi != nullptr);
    REQUIRE(created_wide != nullptr);
    created_wide->Release();
    created_ansi->Release();

    std::vector<wchar_t> system_directory(32'768U);
    const UINT system_directory_length =
        GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    REQUIRE(system_directory_length > 0U);
    REQUIRE(system_directory_length < system_directory.size());
    const auto system_facade_path = std::filesystem::path{system_directory.data()} / L"dinput8.dll";
    HMODULE const system_facade = LoadLibraryExW(system_facade_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (system_facade == nullptr) {
        FAIL("Failed to load the system DirectInput DLL");
    } else {
        const auto system_create = ordinal_export<directinput8_create_fn>(system_facade, 1U);
        REQUIRE(system_create != nullptr);
        IDirectInput8A* native_input{};
        REQUIRE(system_create(GetModuleHandleW(nullptr),
                    DIRECTINPUT_VERSION,
                    IID_IDirectInput8A,
                    reinterpret_cast<LPVOID*>(&native_input),
                    nullptr) == DI_OK);
        REQUIRE(native_input != nullptr);

        enumeration_result input_keyboard{};
        enumeration_result native_keyboard{};
        enumeration_result input_mouse{};
        enumeration_result native_mouse{};
        REQUIRE(enumerate_instance_ids(uninitialized, DI8DEVCLASS_KEYBOARD, input_keyboard));
        REQUIRE(enumerate_instance_ids(native_input, DI8DEVCLASS_KEYBOARD, native_keyboard));
        REQUIRE(enumerate_instance_ids(uninitialized, DI8DEVCLASS_POINTER, input_mouse));
        REQUIRE(enumerate_instance_ids(native_input, DI8DEVCLASS_POINTER, native_mouse));
        CHECK(same_instance_ids(input_keyboard, native_keyboard));
        CHECK(same_instance_ids(input_mouse, native_mouse));
        CHECK(native_input->Release() == 0U);
        CHECK(FreeLibrary(system_facade) != FALSE);
    }

    wide_identity->Release();
    narrow_identity->Release();
    wide->Release();
    factory->Release();

    REQUIRE(FreeLibrary(module) != FALSE);
    // The COM implementation resides in the pinned runtime, so it remains callable.
    IDirectInput8W* after_facade_unload{};
    REQUIRE(uninitialized->QueryInterface(IID_IDirectInput8W,
                reinterpret_cast<void**>(&after_facade_unload)) == S_OK);
    after_facade_unload->Release();
    CHECK(uninitialized->Release() == 0U);
}
