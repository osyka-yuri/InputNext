#include "audio/audio_service.hpp"
#include "audio/native_port.hpp"
#include "audio/native_types.hpp"
#include "detail.hpp"
#include "worker_detail.hpp"

#include <atomic>
#include <optional>

namespace inputnext::audio_detail {

void record_error(audio_slot& slot, const native_error error) noexcept {
    if (error.operation == native_operation::none && error.native_code == 0U) {
        return;
    }
    publish_native_error(slot, error);
}

void wake_control(audio_slot& slot) noexcept {
    if (slot.control_wakeup != nullptr) {
        slot.control_wakeup->notify();
    }
}

void fail_worker(audio_slot& slot,
    const bool retry,
    const std::optional<native_error> error) noexcept {
    if (error.has_value()) {
        record_error(slot, *error);
    }
    slot.retry_after_join.store(retry, std::memory_order_release);
    slot.native_failed.store(true, std::memory_order_release);
    slot.transport_closed.store(true, std::memory_order_release);
    slot.setup.store(setup_phase::rejected, std::memory_order_release);
    wake_control(slot);
}

namespace {
void wait_for_retirement(const audio_slot& slot) noexcept {
    while (!slot.retirement_settled.load(std::memory_order_acquire)) {
        slot.retirement_settled.wait(false, std::memory_order_acquire);
    }
}

void record_cleanup_report(audio_slot& slot, const native_cleanup_report& cleanup) noexcept {
    slot.native_cleanup_complete.store(true, std::memory_order_release);
    if (cleanup.stop.attempted && !cleanup.stop.succeeded) {
        record_error(slot,
            native_error{.operation = native_operation::stop,
                .native_code = cleanup.stop.native_code});
    }
    if (cleanup.buffer_event_close.attempted && !cleanup.buffer_event_close.succeeded) {
        record_error(slot,
            native_error{.operation = native_operation::close_buffer_event,
                .native_code = cleanup.buffer_event_close.native_code});
    }
    if (cleanup.mmcss_revert.attempted && !cleanup.mmcss_revert.succeeded) {
        record_error(slot,
            native_error{.operation = native_operation::revert_mmcss,
                .native_code = cleanup.mmcss_revert.native_code});
    }
}

void finish_native_worker(audio_slot& slot, const native_worker_state& state) noexcept {
    if (state.has_resource &&
        slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::live &&
        !slot.native_failed.load(std::memory_order_acquire)) {
        slot.transport_closed.store(true, std::memory_order_release);
    }
    if (!state.sta_attempted) {
        slot.transport_closed.store(true, std::memory_order_release);
    }
    slot.setup.store(setup_phase::rejected, std::memory_order_release);
    wake_control(slot);
    wait_for_retirement(slot);

    if (state.sta_attempted) {
        const auto cleanup = slot.port->finish_worker_sta();
        if (cleanup.has_value()) {
            record_cleanup_report(slot, *cleanup);
        } else {
            record_error(slot, cleanup.error());
        }
    }
    slot.worker_done.store(true, std::memory_order_release);
    wake_control(slot);
}
} // namespace

void audio_service::implementation::run_native(audio_slot& slot) noexcept {
    native_worker_state state{};
    try {
        if (prepare_native_worker(runtime, *slot.port, slot, state) &&
            !slot.transport_closed.load(std::memory_order_acquire)) {
            run_native_stream(slot,
                *slot.port,
                state.buffer_size,
                state.resource,
                state.generation);
        }
    } catch (...) {
        fail_worker(slot, true);
    }
    finish_native_worker(slot, state);
}
} // namespace inputnext::audio_detail
