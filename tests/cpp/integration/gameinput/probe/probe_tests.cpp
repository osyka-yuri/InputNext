#include "supervisor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>

namespace {

[[nodiscard]] std::filesystem::path current_executable() {
    std::wstring buffer(256U, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{buffer};
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U);
    }
}

[[nodiscard]] std::filesystem::path sibling_executable(const wchar_t* name) {
    const auto current = current_executable();
    return current.empty() ? std::filesystem::path{} : current.parent_path() / name;
}

struct child_run final {
    inputnext::gameinput_detail::owned_process_result process{};
    std::string output;
    std::string errors;
};

[[nodiscard]] child_run run_child(const wchar_t* executable,
    std::vector<std::wstring> arguments,
    std::chrono::milliseconds timeout) {
    const auto path = sibling_executable(executable);
    if (path.empty()) {
        return {.process = {.timed_out = false, .exit_code = 0xFFFF'FFFFUL},
            .output = "Could not locate the current executable.",
            .errors = {}};
    }
    std::ostringstream child_output;
    std::ostringstream child_errors;
    const auto result =
        inputnext::gameinput_detail::run_owned_process({.executable = path,
                                                           .working_directory = path.parent_path(),
                                                           .arguments = std::move(arguments),
                                                           .timeout = timeout},
            {.output = child_output, .errors = child_errors});
    return {.process = result, .output = child_output.str(), .errors = child_errors.str()};
}

} // namespace

TEST_CASE("gameinput.probe.supervisor_owns_child_and_propagates_exit",
    "[integration][gameinput][probe]") {
    const auto successful =
        run_child(L"inputnext_gameinput_probe_child_fixture.exe", {}, std::chrono::seconds{2});
    CHECK_FALSE(successful.process.timed_out);
    CHECK(successful.process.cleanup_confirmed);
    CHECK(successful.process.exit_code == 0U);
    CHECK(successful.output == "fixture=exit code=0\n");

    const auto failed = run_child(L"inputnext_gameinput_probe_child_fixture.exe",
        {L"--exit-code", L"23"},
        std::chrono::seconds{2});
    CHECK_FALSE(failed.process.timed_out);
    CHECK(failed.process.cleanup_confirmed);
    CHECK(failed.process.exit_code == 23U);
    CHECK(failed.output.contains("fixture=exit code=23"));
}

TEST_CASE("gameinput.probe.supervisor_terminates_its_timed_out_owned_child",
    "[integration][gameinput][probe][process]") {
    const auto result = run_child(L"inputnext_gameinput_probe_child_fixture.exe",
        {L"--hang"},
        std::chrono::milliseconds{150});
    CHECK(result.process.timed_out);
    CHECK(result.process.cleanup_confirmed);
    CHECK(result.output.contains("fixture=started"));
    CHECK(result.errors.contains("child=timeout termination_confirmed=1 output_closed=1"));

    const auto flooding = run_child(L"inputnext_gameinput_probe_child_fixture.exe",
        {L"--flood"},
        std::chrono::milliseconds{150});
    CHECK(flooding.process.timed_out);
    CHECK(flooding.process.cleanup_confirmed);
    CHECK_FALSE(flooding.output.empty());
}

TEST_CASE("gameinput.probe.smoke_and_argument_validation", "[integration][gameinput][probe]") {
    const auto executable = sibling_executable(L"inputnext_gameinput_probe.exe");
    REQUIRE_FALSE(executable.empty());

    std::ostringstream child_output;
    std::ostringstream child_errors;
    const auto smoke = inputnext::gameinput_detail::run_owned_process(
        {.executable = executable,
            .working_directory = executable.parent_path(),
            .arguments = {L"--duration-ms", L"25"},
            .timeout = std::chrono::seconds{20}},
        {.output = child_output, .errors = child_errors});
    CHECK_FALSE(smoke.timed_out);
    CHECK(smoke.cleanup_confirmed);
    CHECK(smoke.exit_code == 0U);
    CHECK(child_output.str().contains("start=ok"));
    CHECK(child_output.str().contains("stop=drained"));

    child_output.str({});
    child_output.clear();
    child_errors.str({});
    child_errors.clear();
    const auto invalid = inputnext::gameinput_detail::run_owned_process(
        {.executable = executable,
            .working_directory = executable.parent_path(),
            .arguments = {L"--duration-ms", L"0"},
            .timeout = std::chrono::seconds{2}},
        {.output = child_output, .errors = child_errors});
    CHECK_FALSE(invalid.timed_out);
    CHECK(invalid.cleanup_confirmed);
    CHECK(invalid.exit_code == 2U);
}

TEST_CASE("gameinput.probe.supervisor_forwards_history_option", "[integration][gameinput][probe]") {
    const auto executable = sibling_executable(L"inputnext_gameinput_probe.exe");
    REQUIRE_FALSE(executable.empty());
    std::ostringstream output;
    std::ostringstream errors;
    const auto result = inputnext::gameinput_detail::run_owned_process(
        {.executable = executable,
            .working_directory = executable.parent_path(),
            .arguments = {L"--duration-ms", L"25", L"--history"},
            .timeout = std::chrono::seconds{20}},
        {.output = output, .errors = errors});
    CHECK_FALSE(result.timed_out);
    CHECK(result.cleanup_confirmed);
    CHECK(result.exit_code == 0U);
    CHECK(output.str().contains("start=ok"));
    CHECK(output.str().contains("history_enabled=true"));
    CHECK(output.str().contains("stop=drained"));
}
