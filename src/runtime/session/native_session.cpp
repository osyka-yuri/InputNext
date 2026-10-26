#include "native_session.hpp"

#include "core/core.hpp"
#include "native_session_state.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/xinput.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

namespace inputnext::runtime_detail {

native_session::native_session(std::unique_ptr<implementation> state) noexcept
    : implementation_(std::move(state)) {
}

native_session::~native_session() noexcept {
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

gameinput_result<void> native_session::start() noexcept {
    if (implementation_ == nullptr || implementation_->runtime == nullptr) {
        return std::unexpected{gameinput_failure{.code = gameinput_error::closed}};
    }

    {
        std::scoped_lock lock{implementation_->lifecycle_gate};
        if (implementation_->stop_requested) {
            return std::unexpected{gameinput_failure{.code = gameinput_error::closed}};
        }
        if (implementation_->start_attempted) {
            return std::unexpected{gameinput_failure{.code = gameinput_error::already_started}};
        }
        implementation_->start_attempted = true;
    }

    gameinput_result<void> result{};
    try {
        result = implementation_->runtime->start(std::chrono::steady_clock::now());
    } catch (const std::bad_alloc&) {
        result = std::unexpected{gameinput_failure{.code = gameinput_error::resource_exhausted}};
    } catch (const std::system_error&) {
        result = std::unexpected{gameinput_failure{.code = gameinput_error::native_failure}};
    } catch (...) {
        result = std::unexpected{gameinput_failure{.code = gameinput_error::native_failure}};
    }

    {
        std::scoped_lock lock{implementation_->lifecycle_gate};
        implementation_->start_completed = true;
        implementation_->startup_succeeded = result.has_value();
        if (!result.has_value()) {
            implementation_->startup_failure = result.error();
        }
    }
    implementation_->lifecycle_changed.notify_all();
    return result;
}

void native_session::activate() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    {
        std::scoped_lock lock{implementation_->lifecycle_gate};
        implementation_->activated = true;
    }
    implementation_->lifecycle_changed.notify_all();
}

xinput::service& native_session::presentation() noexcept {
    return implementation_->presentation_service->presentation();
}

core::service& native_session::core() noexcept {
    return *implementation_->core_service;
}

void native_session::shutdown() noexcept {
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

native_session_drain_progress native_session::drain_progress() const noexcept {
    native_session_drain_progress result{};
    if (implementation_ == nullptr) {
        return result;
    }

    const auto& state = *implementation_;
    result.phase = state.drain_phase.load(std::memory_order_acquire);
    const bool started = state.drain_started.load(std::memory_order_acquire);
    if (started) {
        const auto start_ticks = state.drain_started_ticks.load(std::memory_order_relaxed);
        result.elapsed = std::chrono::steady_clock::now().time_since_epoch() -
                         std::chrono::steady_clock::duration{start_ticks};
    }
    result.stop_attempts = state.drain_stop_attempts.load(std::memory_order_relaxed);
    result.exception_count = state.drain_exception_count.load(std::memory_order_relaxed);
    result.resident_audio_slots = state.resident_audio_slots.load(std::memory_order_relaxed);
    result.retained_audio_slots = state.retained_audio_slots.load(std::memory_order_relaxed);
    result.has_last_stop_report = state.has_last_stop_report.load(std::memory_order_acquire);
    if (result.has_last_stop_report) {
        result.last_stop_report = gameinput_stop_report{
            .drained = state.last_stop_drained.load(std::memory_order_relaxed),
            .pending_callback_owners =
                state.last_pending_callback_owners.load(std::memory_order_relaxed),
            .active_metadata_queries =
                state.last_active_metadata_queries.load(std::memory_order_relaxed),
            .active_history_calls = state.last_active_history_calls.load(std::memory_order_relaxed),
            .pending_history_owners =
                state.last_pending_history_owners.load(std::memory_order_relaxed),
            .pending_output_leases =
                state.last_pending_output_leases.load(std::memory_order_relaxed),
            .pending_native_cleanup_owners =
                state.last_pending_native_cleanup_owners.load(std::memory_order_relaxed),
            .native_api_cleanup_complete =
                state.last_native_api_cleanup_complete.load(std::memory_order_relaxed),
        };
    }
    return result;
}

} // namespace inputnext::runtime_detail
