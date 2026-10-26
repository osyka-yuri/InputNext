#include "audio/audio_service.hpp"
#include "audio/native_types.hpp"
#include "detail.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>
#include <utility>

namespace inputnext::audio_detail {
namespace {
using clock_type = std::chrono::steady_clock;
} // namespace

void audio_service::implementation::native_entry(void* const context) noexcept {
    auto& slot = *static_cast<audio_slot*>(context);
    auto& service = *static_cast<implementation*>(slot.service_context);
    service.run_native(slot);
}

bool audio_service::implementation::resource_conflicts(const audio_slot& current,
    const native_resource_key& key) const noexcept {
    for (const auto& slot : slots) {
        if (&slot == &current || !slot.reservation_held || !slot.reserved_key.has_value()) {
            continue;
        }
        if (native_resource_conflicts(*slot.reserved_key, key)) {
            return true;
        }
    }
    return false;
}

void audio_service::implementation::maybe_start_target(
    const gameinput_connection& connection) noexcept {
    if (!connection.target.valid() || has_resident(connection.target)) {
        return;
    }

    const auto cached = connections.backend_progress(connection.target);
    if (cached.has_value() && cached->phase == output::backend_phase::current &&
        cached->body_routes != output::body_route_support::hd) {
        clear_retry(connection.target);
        return;
    }

    const auto retry = std::ranges::find_if(retries, [&connection](const retry_state& value) {
        return value.occupied && value.target == connection.target;
    });
    const auto now = clock_type::now();
    if (retry != retries.end() && retry->not_before > now) {
        return;
    }

    const auto managed = connections.query_backend_progress(connection.target);
    if (!managed.has_value() || managed->phase != output::backend_phase::current ||
        !managed->backend.valid()) {
        schedule_retry(connection.target);
        return;
    }
    if (managed->body_routes != output::body_route_support::hd) {
        clear_retry(connection.target);
        return;
    }

    const auto free = std::ranges::find_if(slots, [](const audio_slot& slot) {
        return slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::vacant;
    });
    if (next_slot_generation == 0U || next_slot_generation == UINT64_MAX) {
        accepting.store(false, std::memory_order_seq_cst);
        stopping.store(true, std::memory_order_seq_cst);
        return;
    }
    if (free == slots.end()) {
        schedule_retry(connection.target);
        return;
    }

    auto port = native_factory->create();
    if (!port.has_value() || *port == nullptr) {
        record_start_failure(audio_start_failure{.target = connection.target,
            .backend = managed->backend,
            .error = port.has_value()
                         ? native_error{.operation = native_operation::create_native_port}
                         : port.error()});
        return;
    }
    if (retry != retries.end()) {
        retry->last_start_failure.reset();
    }

    free->generation = next_slot_generation++;
    free->target = connection.target;
    free->backend = managed->backend;
    free->audio.reset();
    free->candidate_key.reset();
    free->reserved_key.reset();
    free->reservation_held = false;
    free->attachment_obligation = false;
    free->detach_obligation = false;
    free->authority_ready = false;
    free->stop_notified = false;
    free->telemetry_collected = false;
    free->retry_after_join = false;
    free->last_admitted_route.reset();
    free->last_admitted_playback.reset();
    free->last_route_error.reset();
    free->last_metadata_error.store(0U, std::memory_order_relaxed);
    free->has_metadata_error.store(false, std::memory_order_relaxed);
    free->identity_valid_until_ticks.store(0, std::memory_order_relaxed);
    free->worker_done.store(false, std::memory_order_relaxed);
    free->native_cleanup_complete.store(false, std::memory_order_relaxed);
    free->retirement_settled.store(false, std::memory_order_relaxed);
    free->native_failed.store(false, std::memory_order_relaxed);
    free->native_setup_started.store(false, std::memory_order_relaxed);
    free->native_stream_started.store(false, std::memory_order_relaxed);
    free->ready.store(false, std::memory_order_relaxed);
    free->mmcss_registered.store(false, std::memory_order_relaxed);
    free->counters_saturated.store(false, std::memory_order_relaxed);
    free->last_native_error_value.store(0U, std::memory_order_relaxed);
    free->has_native_error.store(false, std::memory_order_relaxed);
    free->admitted_blocks.value.store(0U, std::memory_order_relaxed);
    free->source_frames_consumed.value.store(0U, std::memory_order_relaxed);
    free->accepted_pcm_buffers.value.store(0U, std::memory_order_relaxed);
    free->accepted_pcm_frames.value.store(0U, std::memory_order_relaxed);
    free->accepted_silent_buffers.value.store(0U, std::memory_order_relaxed);
    free->setup.store(setup_phase::empty, std::memory_order_relaxed);
    free->handoff.store(handoff_phase::idle, std::memory_order_relaxed);
    free->next_request_sequence = 1U;
    free->service_context = this;
    free->control_wakeup = &control_wakeup;
    free->transport_closed.store(false, std::memory_order_relaxed);
    free->port = std::move(*port);
    free->lifecycle.store(slot_lifecycle::opening, std::memory_order_release);
    free->worker = worker_launcher->launch(&implementation::native_entry, &*free);
    if (free->worker == nullptr) {
        free->transport_closed.store(true, std::memory_order_release);
        free->port.reset();
        free->lifecycle.store(slot_lifecycle::vacant, std::memory_order_release);
        schedule_retry(connection.target);
    }
}

void audio_service::implementation::process_setup(audio_slot& slot) noexcept {
    const auto setup = slot.setup.load(std::memory_order_acquire);
    const auto lifecycle = slot.lifecycle.load(std::memory_order_acquire);
    if (lifecycle != slot_lifecycle::opening) {
        return;
    }
    if (setup == setup_phase::candidate_ready) {
        reserve_candidate(slot);
    } else if (setup == setup_phase::prepared) {
        attach_prepared(slot);
    }
}

void audio_service::implementation::reserve_candidate(audio_slot& slot) const noexcept {
    if (!slot.candidate_key.has_value() || slot.transport_closed.load(std::memory_order_seq_cst) ||
        !desired(slot.target) || !backend_is_current(slot)) {
        slot.setup.store(setup_phase::rejected, std::memory_order_release);
        close_slot(slot);
        return;
    }
    if (resource_conflicts(slot, *slot.candidate_key)) {
        slot.retry_after_join.store(true, std::memory_order_release);
        slot.setup.store(setup_phase::rejected, std::memory_order_release);
        close_slot(slot);
        return;
    }
    slot.reserved_key = slot.candidate_key;
    slot.reservation_held = true;
    slot.setup.store(setup_phase::granted, std::memory_order_release);
    if (slot.port != nullptr) {
        const auto notified = slot.port->wake_worker();
        if (!notified.signaled) {
            publish_native_error(slot, notified.error);
            close_slot(slot);
        }
    }
}

void audio_service::implementation::attach_prepared(audio_slot& slot) noexcept {
    const auto valid_before_attach = !slot.transport_closed.load(std::memory_order_seq_cst) &&
                                     desired(slot.target) && backend_is_current(slot) &&
                                     slot.reserved_key.has_value() &&
                                     !slot.native_failed.load(std::memory_order_acquire) &&
                                     slot.identity_valid_until_ticks.load(
                                         std::memory_order_acquire) > time_ticks(clock_type::now());
    if (!valid_before_attach) {
        close_slot(slot);
        return;
    }

    const auto attached = route_control.attach(slot.target, output::body_route_support::hd);
    if (!attached.has_value()) {
        slot.last_route_error = attached.error();
        slot.retry_after_join.store(true, std::memory_order_release);
        close_slot(slot);
        return;
    }
    slot.audio = *attached;
    slot.attachment_obligation = true;
    slot.detach_obligation = true;
    if (slot.transport_closed.load(std::memory_order_seq_cst) || !desired(slot.target) ||
        !backend_is_current(slot) ||
        slot.identity_valid_until_ticks.load(std::memory_order_acquire) <=
            time_ticks(clock_type::now())) {
        close_slot(slot);
        return;
    }

    const auto ready = route_control.set_ready(*slot.audio, true);
    if (!ready.has_value()) {
        slot.last_route_error = ready.error();
        slot.retry_after_join.store(true, std::memory_order_release);
        close_slot(slot);
        return;
    }
    slot.authority_ready = true;
    const bool still_current = !slot.transport_closed.load(std::memory_order_seq_cst) &&
                               desired(slot.target) && backend_is_current(slot) &&
                               slot.identity_valid_until_ticks.load(std::memory_order_acquire) >
                                   time_ticks(clock_type::now());
    if (!still_current) {
        close_slot(slot);
        return;
    }
    slot.ready.store(true, std::memory_order_release);
    slot.lifecycle.store(slot_lifecycle::live, std::memory_order_release);
    clear_retry(slot.target);
    if (slot.port != nullptr) {
        static_cast<void>(slot.port->wake_worker());
    }
}
} // namespace inputnext::audio_detail
