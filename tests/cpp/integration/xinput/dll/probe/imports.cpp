#include "support.hpp"

#include <DbgHelp.h>
#include <Windows.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
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
    for (std::size_t index = 0U; index < size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index) {
        const auto& descriptor = imports[index];
        if (descriptor.Name == 0U) {
            break;
        }
        const auto* name = reinterpret_cast<const char*>(image + descriptor.Name);
        if (std::string_view{name} != "xinput1_4.dll") {
            continue;
        }
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
        HMODULE owner{};
        require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    address,
                    &owner) != FALSE,
            "could not identify the resolved companion import owner");
        return owner;
    }
    throw std::runtime_error("facade has no ordinary XInput 1.4 import");
}

} // namespace inputnext::xinput_dll_probe
