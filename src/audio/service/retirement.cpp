#include "audio/audio_service.hpp"
#include "detail.hpp"
#include <atomic>
#include <chrono>
#include <inputnext/output.hpp>

namespace inputnext::audio_detail {
namespace {
using clock_type = std::chrono::steady_clock;
} // namespace

void audio_service::implementation::close_slot(audio_slot& slot) noexcept {
    const auto prior = slot.lifecycle.load(std::memory_order_acquire);
    if (prior == slot_lifecycle::vacant || prior == slot_lifecycle::retained) {
        return;
    }
    const auto identity_deadline = slot.identity_valid_until_ticks.load(std::memory_order_acquire);
    if (identity_deadline != 0 && identity_deadline <= time_ticks(clock_type::now())) {
        slot.retry_after_join.store(true, std::memory_order_release);
    }
    slot.transport_closed.store(true, std::memory_order_seq_cst);
    slot.lifecycle.store(slot_lifecycle::revoking, std::memory_order_release);
    if (!slot.stop_notified && slot.port != nullptr) {
        slot.stop_notified = true;
        const auto stopped = slot.port->request_stop();
        if (!stopped.signaled) {
            publish_native_error(slot, stopped.error);
        }
    }
}

void audio_service::implementation::settle_attachment(audio_slot& slot) noexcept {
    if (!slot.transport_closed.load(std::memory_order_acquire)) {
        return;
    }
    if (slot.audio.has_value() && slot.authority_ready) {
        const auto not_ready = route_control.set_ready(*slot.audio, false);
        if (!not_ready.has_value()) {
            slot.last_route_error = not_ready.error();
        }
        if (not_ready.has_value() || not_ready.error() == output::error::stale_audio) {
            slot.authority_ready = false;
            slot.ready.store(false, std::memory_order_release);
        }
    }
    if (slot.audio.has_value() && slot.detach_obligation) {
        const auto detached = route_control.detach(*slot.audio);
        if (detached.has_value() || detached.error() == output::error::stale_audio) {
            slot.detach_obligation = false;
            slot.attachment_obligation = false;
            slot.authority_ready = false;
            slot.ready.store(false, std::memory_order_release);
        } else {
            slot.last_route_error = detached.error();
        }
    }
    if (!slot.attachment_obligation && !slot.detach_obligation &&
        !slot.retirement_settled.load(std::memory_order_acquire)) {
        slot.retirement_settled.store(true, std::memory_order_release);
        slot.retirement_settled.notify_all();
        slot.lifecycle.store(slot_lifecycle::draining, std::memory_order_release);
        if (slot.port != nullptr) {
            static_cast<void>(slot.port->wake_worker());
        }
    }
}

void audio_service::implementation::collect_worker(audio_slot& slot) noexcept {
    if (!slot.worker_done.load(std::memory_order_acquire)) {
        if (slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::revoking ||
            slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::draining) {
            slot.lifecycle.store(slot_lifecycle::draining, std::memory_order_release);
        }
        return;
    }
    if (slot.worker != nullptr && slot.worker->joinable() && !slot.worker->join()) {
        slot.lifecycle.store(slot_lifecycle::retained, std::memory_order_release);
        return;
    }
    if (slot.worker != nullptr && slot.worker->joinable()) {
        slot.lifecycle.store(slot_lifecycle::retained, std::memory_order_release);
        return;
    }
    if (!slot.native_cleanup_complete.load(std::memory_order_acquire) ||
        slot.attachment_obligation || slot.detach_obligation ||
        !slot.retirement_settled.load(std::memory_order_acquire)) {
        slot.lifecycle.store(slot_lifecycle::retained, std::memory_order_release);
        return;
    }

    const auto target = slot.target;
    const bool should_retry = slot.retry_after_join && desired(target);
    accumulate_retired_telemetry(slot);
    slot.worker.reset();
    slot.port.reset();
    slot.reserved_key.reset();
    slot.candidate_key.reset();
    slot.reservation_held = false;
    slot.audio.reset();
    slot.backend = {};
    slot.target = {};
    slot.service_context = nullptr;
    slot.control_wakeup = nullptr;
    slot.lifecycle.store(slot_lifecycle::vacant, std::memory_order_release);
    if (should_retry) {
        schedule_retry(target);
    } else {
        clear_retry(target);
    }
}
} // namespace inputnext::audio_detail
