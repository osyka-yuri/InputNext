#include "profiles/loader.hpp"

#include "profiles/parser.hpp"
#include <inputnext/output.hpp>

#include <bit>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    std::string text;
    text.reserve(encoded.size());
    for (const auto byte : encoded) {
        text.push_back(std::bit_cast<char>(byte));
    }
    return text;
}

void print_diagnostic(const std::filesystem::path& path,
    const inputnext::profiles::diagnostic& diagnostic) {
    std::cerr << utf8_path(path);
    if (diagnostic.line != 0U) {
        std::cerr << ':' << diagnostic.line << ':' << diagnostic.column;
    }
    std::cerr << ": " << diagnostic.message << '\n';
}

[[nodiscard]] std::string_view describe(
    const std::optional<inputnext::output::trigger_value>& trigger) noexcept {
    if (!trigger.has_value()) {
        return "unset";
    }
    using inputnext::output::trigger_mode;
    const bool zonal = trigger->layout == inputnext::output::trigger_layout::per_zone;
    switch (trigger->mode) {
    case trigger_mode::off:
        return "off";
    case trigger_mode::feedback:
        return zonal ? "curve" : "feedback";
    case trigger_mode::weapon:
        return "weapon";
    case trigger_mode::vibration:
        return zonal ? "vibration_curve" : "vibration";
    }
    return "unknown";
}

void usage() {
    std::cout << "Usage: inputnext_profile_validator <profile.toml>\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto arguments = std::span{argv, static_cast<std::size_t>(argc)};
        if (arguments.size() != 2U) {
            usage();
            return 2;
        }
        const std::wstring_view argument{arguments[1U]};
        if (argument == L"--help" || argument == L"-h") {
            usage();
            return 0;
        }

        const std::filesystem::path path{argument};
        const auto parsed = inputnext::profiles::load_file(path);
        if (!parsed.has_value()) {
            print_diagnostic(path, parsed.error().detail);
            return 1;
        }
        for (const auto& diagnostic : parsed->diagnostics) {
            print_diagnostic(path, diagnostic);
        }
        if (!parsed->diagnostics.empty()) {
            return 1;
        }
        std::cout << "[OK] " << utf8_path(path)
                  << "\nleft_trigger=" << describe(parsed->settings.left_trigger)
                  << "\nright_trigger=" << describe(parsed->settings.right_trigger) << '\n';
        return 0;
    } catch (const std::exception& error) {
        static_cast<void>(std::fprintf(stderr, "Profile validation failed: %s\n", error.what()));
        return 1;
    } catch (...) {
        static_cast<void>(std::fputs("Profile validation failed: unknown exception\n", stderr));
        return 1;
    }
}
