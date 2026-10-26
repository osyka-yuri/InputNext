#include "audio/audio_service.hpp"
#include "detail.hpp"
#include "output/authority/admitted_body_render.hpp"
#include <atomic>
#include <chrono>
#include <optional>
#include <utility>

namespace inputnext::audio_detail {
namespace {
using clock_type = std::chrono::steady_clock;
} // namespace

void audio_service::implementation::process_handoff(audio_slot& slot) noexcept {
    auto phase = slot.handoff.load(std::memory_order_acquire);
    if (phase == handoff_phase::completed) {
        auto expected = handoff_phase::completed;
        phase = slot.handoff.compare_exchange_strong(expected,
                    handoff_phase::idle,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)
                    ? handoff_phase::idle
                    : expected;
    }
    if (phase == handoff_phase::requested) {
        admit_request(slot);
    } else if (slot.transport_closed.load(std::memory_order_seq_cst) ||
               slot.lifecycle.load(std::memory_order_acquire) != slot_lifecycle::live) {
        cancel_unclaimed_handoff(slot);
    }
}

void audio_service::implementation::admit_request(audio_slot& slot) noexcept {
    auto expected = handoff_phase::requested;
    if (!slot.handoff.compare_exchange_strong(expected,
            handoff_phase::admitting,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return;
    }
    const auto request = slot.request;
    std::optional<output::detail::admitted_body_render> admitted = std::nullopt;
    if (!obtain_admission(slot, request, admitted)) {
        admitted.reset();
        slot.handoff.store(handoff_phase::cancelled, std::memory_order_release);
        close_slot(slot);
        if (slot.port != nullptr) {
            static_cast<void>(slot.port->wake_worker());
        }
        return;
    }
    publish_response(slot, request, std::move(admitted));
}

bool audio_service::implementation::obtain_admission(audio_slot& slot,
    const render_request& request,
    std::optional<output::detail::admitted_body_render>& admitted) noexcept {
    const bool request_current =
        request.slot_generation == slot.generation && request.sequence != 0U &&
        slot.audio.has_value() && request.audio == *slot.audio && request.frame_bound != 0U &&
        request.frame_bound <= audio_max_block_frames &&
        slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::live &&
        !slot.transport_closed.load(std::memory_order_seq_cst) &&
        slot.ready.load(std::memory_order_acquire) && accepting.load(std::memory_order_seq_cst) &&
        slot.identity_valid_until_ticks.load(std::memory_order_acquire) >
            time_ticks(clock_type::now());
    if (!request_current || !desired(slot.target) || !backend_is_current(slot)) {
        return false;
    }

    auto result = render_control.admit_current(slot.backend, request.audio, request.frame_bound);
    if (!result.has_value()) {
        return false;
    }
    admitted = std::move(*result);
    return desired(slot.target) && backend_is_current(slot) &&
           !slot.transport_closed.load(std::memory_order_seq_cst) &&
           accepting.load(std::memory_order_seq_cst) &&
           slot.identity_valid_until_ticks.load(std::memory_order_acquire) >
               time_ticks(clock_type::now());
}

void audio_service::implementation::publish_response(audio_slot& slot,
    const render_request& request,
    std::optional<output::detail::admitted_body_render> admitted) noexcept {
    slot.response.slot_generation = request.slot_generation;
    slot.response.sequence = request.sequence;
    slot.response.audio = request.audio;
    slot.response.expires_at = clock_type::now() + audio_offer_lifetime;
    slot.response.admitted = std::move(admitted);
    if (slot.response.admitted.has_value()) {
        slot.last_admitted_route = slot.response.admitted->route;
        slot.last_admitted_playback = slot.response.admitted->playback;
    }
    const bool still_open =
        slot.lifecycle.load(std::memory_order_acquire) == slot_lifecycle::live &&
        accepting.load(std::memory_order_seq_cst) &&
        !slot.transport_closed.load(std::memory_order_seq_cst) &&
        slot.generation == request.slot_generation && slot.audio == request.audio &&
        slot.identity_valid_until_ticks.load(std::memory_order_acquire) >
            time_ticks(clock_type::now()) &&
        slot.handoff.load(std::memory_order_acquire) == handoff_phase::admitting;
    if (!still_open) {
        slot.response.admitted.reset();
        slot.handoff.store(handoff_phase::cancelled, std::memory_order_release);
        close_slot(slot);
    } else {
        slot.handoff.store(handoff_phase::offered, std::memory_order_release);
    }
    if (slot.port != nullptr) {
        static_cast<void>(slot.port->wake_worker());
    }
}

void audio_service::implementation::cancel_unclaimed_handoff(audio_slot& slot) noexcept {
    const auto phase = slot.handoff.load(std::memory_order_acquire);
    if (phase == handoff_phase::requested) {
        auto expected = handoff_phase::requested;
        static_cast<void>(slot.handoff.compare_exchange_strong(expected,
            handoff_phase::cancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire));
    } else if (phase == handoff_phase::offered) {
        auto expected = handoff_phase::offered;
        if (slot.handoff.compare_exchange_strong(expected,
                handoff_phase::reclaiming,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            slot.response.admitted.reset();
            slot.handoff.store(handoff_phase::cancelled, std::memory_order_release);
        }
    }
    auto expected = handoff_phase::cancelled;
    static_cast<void>(slot.handoff.compare_exchange_strong(expected,
        handoff_phase::completed,
        std::memory_order_acq_rel,
        std::memory_order_acquire));
}
} // namespace inputnext::audio_detail
