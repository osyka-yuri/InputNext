#include "support.hpp"

#include "import_policy.hpp"

#include <DbgHelp.h>
#include <Windows.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] HMODULE imported_companion(const HMODULE facade) {
    ULONG size{};
    const auto* imports = static_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        ImageDirectoryEntryToData(facade, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size));
    if (imports == nullptr) {
        throw std::runtime_error("facade has no ordinary import directory");
    }
    const auto* image = reinterpret_cast<const std::byte*>(facade);
    HMODULE companion_owner{};
    for (std::size_t index = 0U; index < size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index) {
        const auto& descriptor = imports[index];
        if (descriptor.Name == 0U) {
            break;
        }
        const auto* name = reinterpret_cast<const char*>(image + descriptor.Name);
        const std::string_view module_name{name};
        constexpr bool address_profile = INPUTNEXT_TEST_ASAN_IMPORTS != 0;
        if (!runtime_dll_import_allowed(module_name, address_profile)) {
            throw std::runtime_error(
                std::string{"facade imports unexpected runtime DLL: "} + std::string{module_name});
        }
        require(descriptor.OriginalFirstThunk != 0U,
            "facade import descriptor has no lookup table");
        {
            const auto* symbols =
                reinterpret_cast<const IMAGE_THUNK_DATA*>(image + descriptor.OriginalFirstThunk);
            for (std::size_t symbol_index = 0U; symbols[symbol_index].u1.AddressOfData != 0U;
                ++symbol_index) {
                if (IMAGE_SNAP_BY_ORDINAL(symbols[symbol_index].u1.Ordinal)) {
                    continue;
                }
                const auto* imported_symbol = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    image + symbols[symbol_index].u1.AddressOfData);
                const std::string_view symbol_name{
                    reinterpret_cast<const char*>(imported_symbol->Name)};
                if (!runtime_symbol_import_allowed(symbol_name, address_profile)) {
                    throw std::runtime_error(
                        std::string{"facade imports unexpected runtime symbol: "} +
                        std::string{symbol_name});
                }
            }
        }
        if (module_name != "xinput1_4.dll") {
            continue;
        }
        require(companion_owner == nullptr, "facade has duplicate XInput 1.4 import descriptors");
        const auto* symbols =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(image + descriptor.OriginalFirstThunk);
        const auto* addresses =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(image + descriptor.FirstThunk);
        require(IMAGE_SNAP_BY_ORDINAL(symbols[0].u1.Ordinal) &&
                    IMAGE_ORDINAL(symbols[0].u1.Ordinal) == 2U && addresses[0].u1.Function != 0U &&
                    addresses[1].u1.Function == 0U,
            "facade must retain exactly the ordinary GetState ordinal import");
        const auto* address =
            std::bit_cast<const wchar_t*>(static_cast<std::uintptr_t>(addresses[0].u1.Function));
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    address,
                    &companion_owner) != FALSE,
            "could not identify the resolved companion import owner");
    }
    if (companion_owner == nullptr) {
        throw std::runtime_error("facade has no ordinary XInput 1.4 import");
    }
    return companion_owner;
}

} // namespace inputnext::xinput_dll_probe
