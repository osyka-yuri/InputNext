#include "native_session_state.hpp"

#include "effects/effects.hpp"
#include "native_session.hpp"
#include "native_session_dependencies.hpp"
#include <inputnext/gameinput_runtime.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>

namespace inputnext::runtime_detail {
namespace {

using namespace std::chrono_literals;

constexpr auto maintenance_period = 4ms;

[[nodiscard]] gameinput_failure unexpected_maintenance_failure() noexcept {
    return gameinput_failure{.code = gameinput_error::native_failure};
}

[[nodiscard]] gameinput_failure maintenance_allocation_failure() noexcept {
    return gameinput_failure{.code = gameinput_error::resource_exhausted};
}

} // namespace

void native_session::implementation::launch(worker_launcher launcher, void* launcher_context) {
    worker = launcher(
        launcher_context,
        [](void* context) noexcept { static_cast<implementation*>(context)->maintenance_loop(); },
        this);
    if (!worker.joinable()) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
            "native session worker launcher returned no worker");
    }
}

void native_session::implementation::maintenance_loop() noexcept {
    bool startup_ok{};
    std::optional<gameinput_failure> startup_error;
    {
        std::unique_lock lock{lifecycle_gate};
        lifecycle_changed.wait(lock,
            [this] { return stop_requested || (activated && start_completed); });
        if (stop_requested) {
            return;
        }
        startup_ok = startup_succeeded;
        startup_error = startup_failure;
    }

    if (!startup_ok) {
        terminate_after_failure(startup_error.value_or(unexpected_maintenance_failure()));
        return;
    }

    for (;;) {
        if (should_stop()) {
            return;
        }

        try {
            if (const auto failure = run_maintenance_cycle(); failure.has_value()) {
                terminate_after_failure(*failure);
                return;
            }
            if (should_stop()) {
                return;
            }
            wait_for_next_cycle();
        } catch (const std::bad_alloc&) {
            terminate_after_failure(maintenance_allocation_failure());
            return;
        } catch (const std::system_error&) {
            terminate_after_failure(unexpected_maintenance_failure());
            return;
        } catch (...) {
            terminate_after_failure(unexpected_maintenance_failure());
            return;
        }
    }
}

std::optional<gameinput_failure> native_session::implementation::run_maintenance_cycle() {
    runtime->maintain(std::chrono::steady_clock::now());
    if (should_stop()) {
        return std::nullopt;
    }

    const auto inventory = runtime->connections();
    {
        std::scoped_lock control_lock{control_gate};
        const auto presented = presentation_service->synchronize(inventory);
        if (!presented.has_value()) {
            return should_stop() ? std::nullopt : std::optional{unexpected_maintenance_failure()};
        }
        const auto applied = profile_application->maintain();
        if (applied.terminal && applied.failure.has_value()) {
            return unexpected_maintenance_failure();
        }
    }
    if (should_stop()) {
        return std::nullopt;
    }

    presentation_service->presentation().maintain_output();
    static_cast<void>(effects_service->maintain());
    const auto synchronized = connections->synchronize(inventory);
    if (!synchronized.has_value()) {
        return synchronized.error();
    }
    audio->synchronize(inventory);
    const auto audio_progress = audio->progress();
    resident_audio_slots.store(audio_progress.resident_slots, std::memory_order_relaxed);
    retained_audio_slots.store(audio_progress.retained_slots, std::memory_order_relaxed);
    output_executor->request_drive();
    if (should_stop()) {
        return std::nullopt;
    }

    // history_time is steady-clock time. QPC frequency was captured during preparation and is
    // supplied to gameinput_xinput, which keeps native history timestamps in their QPC domain.
    presentation_service->maintain_events(std::chrono::steady_clock::now());
    return std::nullopt;
}

void native_session::implementation::wait_for_next_cycle() {
    const auto nominal_deadline = std::chrono::steady_clock::now() + maintenance_period;
    auto deadline = nominal_deadline;
    if (runtime != nullptr) {
        const auto runtime_deadline = runtime->next_deadline();
        if (runtime_deadline.has_value() && *runtime_deadline < deadline) {
            deadline = *runtime_deadline;
        }
    }

    std::unique_lock lock{lifecycle_gate};
    lifecycle_changed.wait_until(lock, deadline, [this] { return stop_requested; });
}

bool native_session::implementation::should_stop() const noexcept {
    std::scoped_lock lock{lifecycle_gate};
    return stop_requested;
}

} // namespace inputnext::runtime_detail
