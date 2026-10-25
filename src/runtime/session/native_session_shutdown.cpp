#include "native_session_state.hpp"

#include "native_session.hpp"
#include <inputnext/gameinput_runtime.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace inputnext::runtime_detail {
namespace {

using namespace std::chrono_literals;

constexpr auto drain_retry_period = 4ms;

} // namespace

void native_session::implementation::request_stop() noexcept {
    {
        std::scoped_lock lock{lifecycle_gate};
        stop_requested = true;
    }
    lifecycle_changed.notify_all();
}

bool native_session::implementation::close_admission_and_presentation() noexcept {
    if (core_service != nullptr) {
        core_service->close_admission();
    }
    bool closed{};
    try {
        std::scoped_lock control_lock{control_gate};
        bool profile_drained = profile_application == nullptr;
        try {
            if (profile_application != nullptr) {
                profile_drained = profile_application->close().pending_contexts == 0U;
            }
        } catch (...) {
            drain_exception_count.fetch_add(1U, std::memory_order_relaxed);
        }
        bool presentation_closed = presentation_service == nullptr;
        try {
            if (presentation_service != nullptr) {
                presentation_service->close();
                presentation_closed = true;
            }
        } catch (...) {
            drain_exception_count.fetch_add(1U, std::memory_order_relaxed);
        }
        closed = profile_drained && presentation_closed;
    } catch (...) {
        drain_exception_count.fetch_add(1U, std::memory_order_relaxed);
    }
    if (runtime_access != nullptr) {
        runtime_access->close_admission();
    }
    if (connections != nullptr) {
        connections->close_admission();
    }
    return closed;
}

void native_session::implementation::drain_owned_domains() noexcept {
    std::scoped_lock drain_lock{drain_gate};
    if (domains_drained) {
        return;
    }

    const auto started_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    drain_started_ticks.store(started_ticks, std::memory_order_relaxed);
    drain_started.store(true, std::memory_order_release);
    drain_phase.store(native_session_drain_phase::closing_admission, std::memory_order_release);

    while (!close_admission_and_presentation()) {
        std::this_thread::sleep_for(drain_retry_period);
    }
    if (output_executor != nullptr) {
        output_executor->request_drive();
    }
    drain_phase.store(native_session_drain_phase::stopping_connections, std::memory_order_release);
    if (connections != nullptr) {
        connections->shutdown();
    }

    drain_phase.store(native_session_drain_phase::stopping_runtime, std::memory_order_release);
    if (runtime != nullptr) {
        for (;;) {
            try {
                drain_stop_attempts.fetch_add(1U, std::memory_order_relaxed);
                const auto report = runtime->stop(std::chrono::steady_clock::now());
                record_stop_report(report);
                if (report.drained) {
                    break;
                }
            } catch (...) {
                drain_exception_count.fetch_add(1U, std::memory_order_relaxed);
                // Keep every borrowed owner alive; only a drained report ends this loop.
                std::this_thread::sleep_for(drain_retry_period);
                continue;
            }
            std::this_thread::sleep_for(drain_retry_period);
        }
    }
    domains_drained = true;
    drain_phase.store(native_session_drain_phase::drained, std::memory_order_release);
}

void native_session::implementation::record_stop_report(
    const gameinput_stop_report& report) noexcept {
    last_stop_drained.store(report.drained, std::memory_order_relaxed);
    last_pending_callback_owners.store(report.pending_callback_owners, std::memory_order_relaxed);
    last_active_metadata_queries.store(report.active_metadata_queries, std::memory_order_relaxed);
    last_active_history_calls.store(report.active_history_calls, std::memory_order_relaxed);
    last_pending_history_owners.store(report.pending_history_owners, std::memory_order_relaxed);
    last_pending_output_leases.store(report.pending_output_leases, std::memory_order_relaxed);
    last_pending_native_cleanup_owners.store(report.pending_native_cleanup_owners,
        std::memory_order_relaxed);
    last_native_api_cleanup_complete.store(report.native_api_cleanup_complete,
        std::memory_order_relaxed);
    has_last_stop_report.store(true, std::memory_order_release);
}

void native_session::implementation::terminate_after_failure(
    const gameinput_failure failure) noexcept {
    {
        std::scoped_lock lock{lifecycle_gate};
        if (startup_succeeded && !maintenance_failure.has_value()) {
            maintenance_failure = failure;
        }
        stop_requested = true;
    }
    lifecycle_changed.notify_all();
    static_cast<void>(close_admission_and_presentation());
    drain_owned_domains();
}

void native_session::implementation::shutdown() noexcept {
    std::scoped_lock shutdown_lock{shutdown_gate};
    if (shutdown_finished) {
        return;
    }

    request_stop();
    static_cast<void>(close_admission_and_presentation());
    {
        std::unique_lock lock{lifecycle_gate};
        if (start_attempted && !start_completed) {
            lifecycle_changed.wait(lock, [this] { return start_completed; });
        }
    }
    if (worker.joinable()) {
        worker.join();
    }

    drain_owned_domains();

    core_service.reset();
    profile_application.reset();
    if (presentation_service != nullptr) {
        presentation_service.reset();
    }
    if (connections != nullptr) {
        connections.reset();
    }
    output_executor.reset();
    runtime_access.reset();
    runtime.reset();
    output_composition.reset();
    if (focus_observer != nullptr) {
        focus_observer->stop();
        focus_observer.reset();
    }
    registry.shutdown();

    shutdown_finished = true;
}

} // namespace inputnext::runtime_detail
