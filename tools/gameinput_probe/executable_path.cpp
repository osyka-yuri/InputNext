#include "executable_path.hpp"
#include <filesystem>

#include <Windows.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace inputnext::gameinput_detail {

std::filesystem::path current_executable_path() {
    std::vector<wchar_t> buffer(256U);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "GetModuleFileNameW");
        }
        if (length < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), length}};
        }
        if (buffer.size() >= 32'768U) {
            throw std::length_error("The executable path exceeds the Windows path limit.");
        }
        buffer.resize(buffer.size() * 2U);
    }
}

} // namespace inputnext::gameinput_detail
