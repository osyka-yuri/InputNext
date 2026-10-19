#include "worker.hpp"

#include "supervisor.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool parse_duration(std::wstring_view text, std::uint32_t& duration_ms) noexcept {
    if (text.empty()) {
        return false;
    }

    std::uint32_t parsed{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = (parsed * 10U) + digit;
    }

    if (parsed == 0U || parsed > 300'000U) {
        return false;
    }
    duration_ms = parsed;
    return true;
}

[[nodiscard]] int usage() {
    std::wcerr << L"Usage: inputnext_gameinput_probe [--duration-ms N] [--require-device] "
                  L"[--require-reading] [--history] [--require-history]\n"
                  L"N must be between 1 and 300000 milliseconds.\n";
    return 2;
}

[[nodiscard]] bool parse_options(int argc,
    wchar_t** argv,
    int first_argument,
    inputnext::gameinput_detail::probe_options& options) {
    for (int index = first_argument; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--require-device") {
            options.require_device = true;
        } else if (argument == L"--require-reading") {
            options.require_reading = true;
        } else if (argument == L"--history") {
            options.history = true;
        } else if (argument == L"--require-history") {
            options.history = true;
            options.require_history = true;
        } else if (argument == L"--duration-ms" && index + 1 < argc) {
            if (!parse_duration(argv[++index], options.duration_ms)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) try {
    const bool worker = argc >= 2 && std::wstring_view{argv[1]} == L"--inputnext-worker";
    inputnext::gameinput_detail::probe_options options{};
    if (!parse_options(argc, argv, worker ? 2 : 1, options)) {
        return usage();
    }
    if (worker) {
        return inputnext::gameinput_detail::run_probe_worker(options, std::cout);
    }

    const auto result = inputnext::gameinput_detail::run_supervised_probe(options,
        {.output = std::cout, .errors = std::cerr});
    return result.exit_code;
} catch (const std::exception& error) {
    static_cast<void>(std::fprintf(stderr, "gameinput_probe_error=%s\n", error.what()));
    return 1;
} catch (...) {
    static_cast<void>(std::fputs("gameinput_probe_error=unknown exception\n", stderr));
    return 1;
}
