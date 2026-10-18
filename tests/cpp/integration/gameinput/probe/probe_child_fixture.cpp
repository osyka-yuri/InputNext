#include <Windows.h>
#include <cstdio>
#include <exception>

#include <array>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>

int wmain(int argc, wchar_t** argv) try {
    if (argc == 2 && std::wstring_view{argv[1]} == L"--hang") {
        std::cout << "fixture=started\n" << std::flush;
        Sleep(INFINITE);
        return 0;
    }

    if (argc == 2 && std::wstring_view{argv[1]} == L"--flood") {
        const std::array<char, 4096U> block{};
        while (true) {
            std::cout.write(block.data(), static_cast<std::streamsize>(block.size()));
            std::cout.flush();
        }
    }

    if (argc == 3 && std::wstring_view{argv[1]} == L"--exit-code") {
        wchar_t* end{};
        const std::wstring_view text{argv[2]};
        const std::wstring terminated{text};
        const auto code = std::wcstoul(terminated.c_str(), &end, 10);
        if (end == terminated.c_str() || end != terminated.c_str() + terminated.size() ||
            code > 255UL) {
            return 2;
        }
        std::cout << "fixture=exit code=" << code << '\n';
        return static_cast<int>(code);
    }

    std::cout << "fixture=exit code=0\n";
    return 0;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "probe_fixture_error=%s\n", error.what()));
    return 2;
} catch (...) {
    static_cast<void>(std::fputs("probe_fixture_error=unknown exception\n", stderr));
    return 2;
}
