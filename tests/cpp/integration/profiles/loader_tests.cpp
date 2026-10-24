#include "profiles/loader.hpp"

#include "filesystem/temporary_directory.hpp"
#include "profiles/parser.hpp"
#include "values/required_value.hpp"
#include <inputnext/output.hpp>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

using namespace inputnext::profiles;
using inputnext::test::required_value;

namespace {

using inputnext::test::temporary_directory;

class current_path_guard final {
public:
    explicit current_path_guard(const std::filesystem::path& replacement)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(replacement);
    }

    current_path_guard(const current_path_guard&) = delete;
    current_path_guard& operator=(const current_path_guard&) = delete;
    current_path_guard(current_path_guard&&) = delete;
    current_path_guard& operator=(current_path_guard&&) = delete;

    ~current_path_guard() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
    }

private:
    std::filesystem::path previous_;
};

class file_handle final {
public:
    explicit file_handle(const std::filesystem::path& path,
        const DWORD access = GENERIC_READ,
        const DWORD sharing = 0U)
        : handle_(CreateFileW(path.c_str(),
              access,
              sharing,
              nullptr,
              OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL,
              nullptr)) {}

    file_handle(const file_handle&) = delete;
    file_handle& operator=(const file_handle&) = delete;
    file_handle(file_handle&&) = delete;
    file_handle& operator=(file_handle&&) = delete;

    ~file_handle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

void write_file(const std::filesystem::path& path, const std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("profiles.loader_resolves_the_executable_sibling_independent_of_cwd",
    "[integration][profiles]") {
    temporary_directory temporary;
    const auto executable_directory = temporary.path() / "game";
    const auto working_directory = temporary.path() / "working";
    std::filesystem::create_directories(executable_directory);
    std::filesystem::create_directories(working_directory);
    write_file(executable_directory / "inputnext.toml",
        "[effects.right_trigger]\nmode = 'feedback'\n");
    write_file(working_directory / "inputnext.toml",
        "[effects.right_trigger]\nmode = 'weapon'\nstart = 0.25\nend = 0.75\n");

    const auto original_directory = std::filesystem::current_path();
    {
        const current_path_guard guard(working_directory);
        const auto loaded = load_for_executable(executable_directory / "game.exe");
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->settings.right_trigger.has_value());
        CHECK(required_value(loaded->settings.right_trigger).mode ==
              inputnext::output::trigger_mode::feedback);
        CHECK(loaded->diagnostics.empty());

        const auto no_sibling_profile =
            load_for_executable(temporary.path() / "missing" / "game.exe");
        REQUIRE_FALSE(no_sibling_profile.has_value());
        CHECK(no_sibling_profile.error().code == load_error::not_found);
    }
    CHECK(std::filesystem::current_path() == original_directory);
}

TEST_CASE("profiles.loader_distinguishes_missing_invalid_and_non_file_paths",
    "[integration][profiles]") {
    temporary_directory temporary;

    const auto missing = load_file(temporary.path() / "missing.toml");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == load_error::not_found);

    const auto empty = load_file({});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == load_error::invalid_path);

    const auto directory = load_file(temporary.path());
    REQUIRE_FALSE(directory.has_value());
    CHECK(directory.error().code == load_error::io_error);

    const auto relative_executable = load_for_executable("relative/game.exe");
    REQUIRE_FALSE(relative_executable.has_value());
    CHECK(relative_executable.error().code == load_error::invalid_path);

    const auto empty_executable = load_for_executable({});
    REQUIRE_FALSE(empty_executable.has_value());
    CHECK(empty_executable.error().code == load_error::invalid_path);
}

TEST_CASE("profiles.loader_reports_a_file_locked_against_reading", "[integration][profiles]") {
    temporary_directory temporary;
    const auto path = temporary.path() / "locked.toml";
    write_file(path, "[effects.left_trigger]\nmode = 'off'\n");
    const file_handle exclusive_handle(path);
    REQUIRE(exclusive_handle.valid());

    const auto loaded = load_file(path);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::io_error);
}

TEST_CASE("profiles.loader_does_not_read_an_active_writers_file", "[integration][profiles]") {
    temporary_directory temporary;
    const auto path = temporary.path() / "being-edited.toml";
    write_file(path, "[effects.left_trigger]\nmode = 'off'\n");
    const file_handle writer{path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE};
    REQUIRE(writer.valid());

    const auto loaded = load_file(path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::io_error);
}

TEST_CASE("profiles.loader_keeps_semantic_diagnostics_and_reports_toml_locations",
    "[integration][profiles]") {
    temporary_directory temporary;
    const auto semantic_file = temporary.path() / "semantic.toml";
    write_file(semantic_file,
        "[effects.left_trigger]\nmode = 'feedback'\nstrength = true\n"
        "[effects.right_trigger]\nmode = 'vibration'\nfrequency = 120\n");

    auto loaded = load_file(semantic_file);
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->diagnostics.empty());
    REQUIRE(loaded->settings.right_trigger.has_value());
    CHECK(loaded->settings.left_trigger == inputnext::output::trigger_value{});
    CHECK(required_value(loaded->settings.right_trigger).mode ==
          inputnext::output::trigger_mode::vibration);
    CHECK(loaded->diagnostics.front().line == 3U);

    const auto syntax_file = temporary.path() / "syntax.toml";
    write_file(syntax_file, "[effects.left_trigger]\nmode = [\n");
    loaded = load_file(syntax_file);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::invalid_toml);
    CHECK(loaded.error().detail.line > 0U);
    CHECK(loaded.error().detail.column > 0U);
}

TEST_CASE("profiles.loader_enforces_the_shared_byte_limit", "[integration][profiles]") {
    temporary_directory temporary;
    std::string at_limit{"#"};
    at_limit.resize(maximum_profile_bytes, ' ');
    const auto accepted_path = temporary.path() / "at-limit.toml";
    write_file(accepted_path, at_limit);
    auto loaded = load_file(accepted_path);
    REQUIRE(loaded.has_value());

    at_limit.push_back(' ');
    const auto oversized_path = temporary.path() / "oversized.toml";
    write_file(oversized_path, at_limit);
    loaded = load_file(oversized_path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == load_error::too_large);
}

TEST_CASE("profiles.loader_accepts_caller_relative_files", "[integration][profiles]") {
    temporary_directory temporary;
    const auto previous_directory = std::filesystem::current_path();
    const auto working_directory = temporary.path() / "relative";
    std::filesystem::create_directories(working_directory);
    write_file(working_directory / "profile.toml", "[effects.left_trigger]\nmode = 'off'\n");

    {
        const current_path_guard guard(working_directory);
        const auto loaded = load_file("profile.toml");
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->settings.left_trigger.has_value());
        CHECK(loaded->settings.left_trigger == inputnext::output::trigger_value{});
    }
    CHECK(std::filesystem::current_path() == previous_directory);
}
