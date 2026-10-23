#include "native_session_test_support.hpp"

#include "runtime/session/native_session.hpp"
#include "supervisor.hpp"

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace inputnext::runtime_session_test_support {
namespace {

[[nodiscard]] std::filesystem::path current_executable() {
    std::wstring buffer(256U, L'\0');
    for (;;) {
        const auto length =
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

} // namespace

TEST_CASE("runtime.session.persistent_native_cleanup_child", "[.owned-child]") {
    session_fixture fixture;
    if (fixture.session == nullptr || fixture.native == nullptr) {
        std::cout << "owned_session_setup=failed\n" << std::flush;
        for (;;) {
            std::this_thread::sleep_for(1s);
        }
    }

    fixture.native->unregister_always_fails.store(true, std::memory_order_release);
    fixture.native->fake->fail_next_watch_registration_with_partial_token();
    const auto startup = fixture.session->start();
    if (startup.has_value()) {
        std::cout << "owned_session_start=unexpected_success\n" << std::flush;
    }
    fixture.session->activate();

    const std::thread shutdown_thread{[&fixture] { fixture.session->shutdown(); }};
    const auto ready_deadline = std::chrono::steady_clock::now() + 3s;
    auto first = fixture.session->drain_progress();
    while (std::chrono::steady_clock::now() < ready_deadline &&
           (first.phase != native_session_drain_phase::stopping_runtime ||
               !first.has_last_stop_report ||
               first.last_stop_report.pending_callback_owners == 0U || first.stop_attempts == 0U)) {
        std::this_thread::sleep_for(2ms);
        first = fixture.session->drain_progress();
    }

    std::this_thread::sleep_for(100ms);
    const auto second = fixture.session->drain_progress();
    const auto native_destructions =
        fixture.destruction_counts->native.load(std::memory_order_acquire);
    const auto metadata_destructions =
        fixture.destruction_counts->metadata.load(std::memory_order_acquire);
    const auto focus_destructions =
        fixture.destruction_counts->focus.load(std::memory_order_acquire);
    const bool evidence_ready =
        !startup.has_value() && first.phase == native_session_drain_phase::stopping_runtime &&
        first.has_last_stop_report && first.last_stop_report.pending_callback_owners > 0U &&
        first.stop_attempts > 0U && second.stop_attempts > first.stop_attempts &&
        second.elapsed > first.elapsed && second.has_last_stop_report &&
        second.last_stop_report.pending_callback_owners > 0U && native_destructions == 0U &&
        metadata_destructions == 0U && focus_destructions == 0U;
    std::cout << "owned_session_evidence=" << (evidence_ready ? "ready" : "failed")
              << " attempts_advanced=" << (second.stop_attempts > first.stop_attempts ? 1 : 0)
              << " elapsed_advanced=" << (second.elapsed > first.elapsed ? 1 : 0)
              << " drained=" << (second.last_stop_report.drained ? 1 : 0)
              << " pending_callback_owners=" << second.last_stop_report.pending_callback_owners
              << " destructions=" << native_destructions << ',' << metadata_destructions << ','
              << focus_destructions << '\n'
              << std::flush;

    // The parent owns this child and terminates it after collecting persistent-drain evidence.
    static_cast<void>(shutdown_thread);
    for (;;) {
        std::this_thread::sleep_for(1s);
    }
}

TEST_CASE("runtime.session.persistent_native_cleanup_retains_owned_graph_until_child_termination",
    "[integration][runtime][session][shutdown][process]") {
    const auto executable = current_executable();
    REQUIRE_FALSE(executable.empty());

    std::ostringstream output;
    std::ostringstream errors;
    const auto result = inputnext::gameinput_detail::run_owned_process(
        {.executable = executable,
            .working_directory = executable.parent_path(),
            .arguments = {L"runtime.session.persistent_native_cleanup_child"},
            .timeout = 5s},
        {.output = output, .errors = errors});

    CHECK(result.timed_out);
    CHECK(result.cleanup_confirmed);
    CHECK(output.str().contains("owned_session_evidence=ready"));
    CHECK(output.str().contains("attempts_advanced=1 elapsed_advanced=1"));
    CHECK(output.str().contains("drained=0 pending_callback_owners=1 destructions=0,0,0"));
    CHECK(errors.str().contains("child=timeout termination_confirmed=1 output_closed=1"));
}

} // namespace inputnext::runtime_session_test_support
