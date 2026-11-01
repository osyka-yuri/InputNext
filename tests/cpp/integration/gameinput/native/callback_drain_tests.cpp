#include "gameinput_probe/process/supervisor.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path current_executable_directory() {
    std::wstring buffer(256U, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer}.parent_path();
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

} // namespace

TEST_CASE("gameinput.native.unexpected_exception_preserves_active_callback_owners",
    "[integration][gameinput][native][process][exception]") {
    const auto helper = current_executable_directory() / L"inputnext_gameinput_native_worker.exe";
    std::ostringstream output;
    std::ostringstream errors;
    const auto result = inputnext::gameinput_detail::run_owned_process(
        {.executable = helper,
            .working_directory = helper.parent_path(),
            .arguments = {L"--verify-unwind-guard"},
            .timeout = std::chrono::seconds{5}},
        {.output = output, .errors = errors});
    INFO(output.str());
    INFO(errors.str());
    CHECK_FALSE(result.timed_out);
    CHECK(result.cleanup_confirmed);
    CHECK(result.exit_code == 13U);
    CHECK(
        output.str() == "reading_callback_entered=1 worker_joinable=1 reading_callback_exited=0\n");
    CHECK(errors.str().empty());
}

TEST_CASE("gameinput.native.unregister_drains_an_entered_callback",
    "[integration][gameinput][native][hardware]") {
    const auto directory = current_executable_directory();
    REQUIRE_FALSE(directory.empty());
    const auto helper = directory / L"inputnext_gameinput_native_worker.exe";

    std::ostringstream output;
    std::ostringstream errors;
    const auto result = inputnext::gameinput_detail::run_owned_process(
        {.executable = helper,
            .working_directory = helper.parent_path(),
            .arguments = {},
            .timeout = std::chrono::seconds{20}},
        {.output = output, .errors = errors});

    INFO(output.str());
    INFO(errors.str());
    CHECK_FALSE(result.timed_out);
    CHECK(result.cleanup_confirmed);
    CHECK(result.exit_code == 0U);
    const auto result_text = output.str();
    CHECK(result_text.contains("reading_callback_entered=1"));
    CHECK(result_text.contains("unregister_pending=1"));
    CHECK(result_text.contains("reading_unregister_succeeded=1"));
    CHECK(result_text.contains("reading_callback_exited=1"));
    CHECK(result_text.contains("device_watch_unregistered=1"));
}
