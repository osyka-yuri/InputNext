#include "audio/audio_service.hpp"
#include "detail.hpp"
#include "worker_detail.hpp"

#include "audio/native_port.hpp"
#include "audio/native_types.hpp"
#include "output/authority/admitted_body_render.hpp"

#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace inputnext::audio_detail {
using clock_type = std::chrono::steady_clock;

namespace {
[[nodiscard]] std::optional<std::uint32_t>
frame_availability(native_port& port, audio_slot& slot, const std::uint32_t capacity) noexcept {
    const auto padding = port.get_current_padding();
    if (!padding.has_value()) {
        record_error(slot, padding.error());
        return std::nullopt;
    }
    if (*padding > capacity) {
        record_error(slot,
            native_error{.operation = native_operation::get_current_padding, .native_code = 0U});
        return std::nullopt;
    }
    return std::min<std::uint32_t>(capacity - *padding, audio_max_block_frames);
}

void complete_handoff(audio_slot& slot) noexcept {
    const auto phase = slot.handoff.load(std::memory_order_acquire);
    if (phase == handoff_phase::claimed || phase == handoff_phase::cancelled) {
        slot.handoff.store(handoff_phase::completed, std::memory_order_release);
    }
    wake_control(slot);
}

struct claimed_response final {
    bool claimed{};
    bool expired{};
    std::optional<output::detail::admitted_body_render> admitted = std::nullopt;
};

[[nodiscard]] claimed_response claim_response(audio_slot& slot,
    const output::audio_instance expected_audio) noexcept {
    auto expected = handoff_phase::offered;
    if (!slot.handoff.compare_exchange_strong(expected,
            handoff_phase::claimed,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {};
    }
    claimed_response result{.claimed = true};
    const auto& response = slot.response;
    if (response.slot_generation != slot.generation || response.sequence != slot.request.sequence ||
        response.audio != expected_audio || response.audio != slot.request.audio) {
        result.expired = true;
        slot.response.admitted.reset();
        return result;
    }
    result.expired = clock_type::now() >= response.expires_at;
    if (result.expired) {
        slot.response.admitted.reset();
    }
    if (!result.expired && response.admitted.has_value()) {
        result.admitted.emplace(std::move(*slot.response.admitted));
        slot.response.admitted.reset();
    }
    return result;
}

struct acquired_offer final {
    std::optional<output::detail::admitted_body_render> admitted = std::nullopt;
    bool completed_without_admission{};
    bool expired{};
    bool pending_buffer_event{};
    bool native_failure{};
};

struct render_frame_limits final {
    std::uint32_t capacity{};
    std::uint32_t requested_frames{};
};

void reclaim_offer(audio_slot& slot) noexcept {
    auto expected = handoff_phase::offered;
    if (slot.handoff.compare_exchange_strong(expected,
            handoff_phase::reclaiming,
            std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
        slot.response.admitted.reset();
        slot.handoff.store(handoff_phase::completed, std::memory_order_release);
        return;
    }
    expected = handoff_phase::requested;
    if (slot.handoff.compare_exchange_strong(expected,
            handoff_phase::cancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        complete_handoff(slot);
    }
}

[[nodiscard]] bool resolve_offer(audio_slot& slot,
    const std::atomic<bool>& accepting,
    const output::audio_instance audio,
    acquired_offer& result) noexcept {
    const auto phase = slot.handoff.load(std::memory_order_acquire);
    if (phase == handoff_phase::offered) {
        auto claimed = claim_response(slot, audio);
        if (!claimed.claimed) {
            return false;
        }
        result.expired = claimed.expired;
        const bool still_open = accepting.load(std::memory_order_seq_cst) &&
                                !slot.transport_closed.load(std::memory_order_seq_cst);
        if (claimed.admitted.has_value() && still_open) {
            result.admitted.emplace(std::move(*claimed.admitted));
        } else {
            claimed.admitted.reset();
            result.completed_without_admission = !claimed.expired && still_open;
            result.expired = result.expired || !still_open;
        }
        if (claimed.expired || !still_open || !result.admitted.has_value()) {
            complete_handoff(slot);
        }
        return true;
    }
    if (phase == handoff_phase::cancelled) {
        result.expired = true;
        complete_handoff(slot);
        return true;
    }
    if (phase == handoff_phase::completed || phase == handoff_phase::idle) {
        result.completed_without_admission = true;
        return true;
    }
    return false;
}

[[nodiscard]] bool wait_for_offer_activity(native_port& port,
    audio_slot& slot,
    const std::chrono::steady_clock::time_point deadline,
    acquired_offer& result) noexcept {
    const auto now = clock_type::now();
    if (now >= deadline) {
        return false;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto wait = port.wait_for_activity(std::max(std::chrono::milliseconds{1}, remaining));
    if (wait.outcome == native_wait_outcome::buffer_ready) {
        result.pending_buffer_event = true;
    } else if (wait.outcome == native_wait_outcome::stop_requested) {
        result.expired = true;
        return true;
    } else if (wait.outcome == native_wait_outcome::failed) {
        record_error(slot, wait.error);
        result.expired = true;
        result.native_failure = true;
        return true;
    }
    return false;
}

[[nodiscard]] acquired_offer await_offer(native_port& port,
    audio_slot& slot,
    const std::atomic<bool>& accepting,
    const output::audio_instance audio) noexcept {
    acquired_offer result{};
    const auto deadline = clock_type::now() + audio_offer_lifetime;
    for (;;) {
        if (slot.transport_closed.load(std::memory_order_seq_cst) ||
            !accepting.load(std::memory_order_seq_cst)) {
            reclaim_offer(slot);
            result.expired = true;
            return result;
        }
        if (resolve_offer(slot, accepting, audio, result)) {
            return result;
        }

        const auto now = clock_type::now();
        if (now >= deadline) {
            reclaim_offer(slot);
            result.expired = true;
            wake_control(slot);
            return result;
        }
        if (wait_for_offer_activity(port, slot, deadline, result)) {
            return result;
        }
    }
}

[[nodiscard]] bool
submit_silence(native_port& port, audio_slot& slot, const std::uint32_t capacity) noexcept {
    const auto available = frame_availability(port, slot, capacity);
    if (!available.has_value()) {
        return false;
    }
    const auto frames = *available;
    if (frames == 0U) {
        return true;
    }
    const auto buffer = port.acquire_render_buffer(frames);
    if (!buffer.has_value()) {
        record_error(slot, buffer.error());
        return false;
    }
    const auto released = port.release_render_buffer(native_buffer_disposition::silent);
    if (!released.has_value()) {
        record_error(slot, released.error());
        return false;
    }
    slot.accepted_silent_buffers.add(1U);
    return true;
}

[[nodiscard]] bool submit_render(native_port& port,
    audio_slot& slot,
    const std::atomic<bool>& accepting,
    const render_frame_limits limits,
    output::detail::admitted_body_render admitted) noexcept {
    const auto frames_before = frame_availability(port, slot, limits.capacity);
    if (!frames_before.has_value()) {
        return false;
    }
    const auto frames = std::min(*frames_before, limits.requested_frames);
    if (frames == 0U) {
        return true;
    }

    auto buffer = port.acquire_render_buffer(frames);
    if (!buffer.has_value()) {
        record_error(slot, buffer.error());
        return false;
    }

    if (!accepting.load(std::memory_order_seq_cst) ||
        slot.transport_closed.load(std::memory_order_seq_cst) ||
        slot.lifecycle.load(std::memory_order_acquire) != slot_lifecycle::live ||
        !slot.ready.load(std::memory_order_acquire) ||
        time_ticks(clock_type::now()) >=
            slot.identity_valid_until_ticks.load(std::memory_order_acquire)) {
        const auto released = port.release_render_buffer(native_buffer_disposition::silent);
        if (!released.has_value()) {
            record_error(slot, released.error());
            return false;
        }
        slot.accepted_silent_buffers.add(1U);
        return true;
    }

    const auto submission = std::move(admitted.permit).render_for_submission(*buffer);
    const auto rendered = submission.result();
    slot.admitted_blocks.add(1U);
    slot.source_frames_consumed.add(rendered.source_frames_consumed);
    auto disposition = native_buffer_disposition::audio;
    if (!accepting.load(std::memory_order_seq_cst) ||
        slot.transport_closed.load(std::memory_order_seq_cst) ||
        rendered.source_frames_consumed == 0U) {
        disposition = native_buffer_disposition::silent;
    }
    const auto released = port.release_render_buffer(disposition);
    if (!released.has_value()) {
        record_error(slot, released.error());
        return false;
    }
    if (disposition == native_buffer_disposition::silent) {
        slot.accepted_silent_buffers.add(1U);
    } else {
        slot.accepted_pcm_buffers.add(1U);
        slot.accepted_pcm_frames.add(rendered.frames_rendered);
    }
    return true;
}

struct stream_schedule final {
    clock_type::time_point last_buffer_event{clock_type::now()};
    clock_type::time_point next_identity_refresh{
        last_buffer_event + audio_identity_refresh_interval};
};

[[nodiscard]] bool maintain_stream_identity(native_port& port,
    gameinput_runtime& runtime,
    audio_slot& slot,
    const native_resource_key& resource,
    const std::uint64_t generation,
    stream_schedule& schedule) noexcept {
    const auto now = clock_type::now();
    if (time_ticks(now) >= slot.identity_valid_until_ticks.load(std::memory_order_acquire)) {
        fail_worker(slot,
            true,
            native_error{.operation = native_operation::get_endpoint_id, .native_code = 0U});
        return false;
    }
    if (now < schedule.next_identity_refresh) {
        return true;
    }
    const auto identity = qualify_fresh_identity(port, runtime, slot, resource, generation);
    if (!identity.has_value()) {
        fail_worker(slot, true, identity.error());
        return false;
    }
    const auto interval = *identity == identity_refresh::refreshed ? audio_identity_refresh_interval
                                                                   : audio_identity_retry_interval;
    schedule.next_identity_refresh = clock_type::now() + interval;
    return true;
}

[[nodiscard]] bool
wait_for_stream_buffer(native_port& port, audio_slot& slot, stream_schedule& schedule) noexcept {
    const auto event_deadline = schedule.last_buffer_event + audio_native_event_timeout;
    const auto deadline = std::min(event_deadline, schedule.next_identity_refresh);
    const auto now = clock_type::now();
    const auto wait_ms = std::max(std::chrono::milliseconds{1},
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
    const auto activity = port.wait_for_activity(wait_ms);
    switch (activity.outcome) {
    case native_wait_outcome::buffer_ready:
        schedule.last_buffer_event = clock_type::now();
        return true;
    case native_wait_outcome::worker_wakeup:
        return false;
    case native_wait_outcome::timeout:
        if (clock_type::now() >= event_deadline) {
            fail_worker(slot,
                true,
                native_error{.operation = native_operation::wait, .native_code = 0U});
        }
        return false;
    case native_wait_outcome::stop_requested:
        slot.transport_closed.store(true, std::memory_order_release);
        wake_control(slot);
        return false;
    case native_wait_outcome::failed:
    default:
        fail_worker(slot, true, activity.error);
        return false;
    }
}

void submit_silence_or_fail(native_port& port,
    audio_slot& slot,
    const std::uint32_t capacity) noexcept {
    if (!submit_silence(port, slot, capacity)) {
        fail_worker(slot, true, latest_native_error(slot));
    }
}

enum class request_status : std::uint8_t {
    sent,
    unavailable,
    failed,
};

[[nodiscard]] request_status begin_render_request(audio_slot& slot,
    const output::audio_instance audio,
    const std::uint32_t frames) noexcept {
    auto phase = slot.handoff.load(std::memory_order_acquire);
    if (phase == handoff_phase::completed) {
        auto expected = handoff_phase::completed;
        static_cast<void>(slot.handoff.compare_exchange_strong(expected,
            handoff_phase::idle,
            std::memory_order_acq_rel,
            std::memory_order_acquire));
        phase = slot.handoff.load(std::memory_order_acquire);
    }
    if (phase == handoff_phase::offered && clock_type::now() >= slot.response.expires_at) {
        auto expected = handoff_phase::offered;
        if (slot.handoff.compare_exchange_strong(expected,
                handoff_phase::reclaiming,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            slot.response.admitted.reset();
            slot.handoff.store(handoff_phase::completed, std::memory_order_release);
            wake_control(slot);
        }
        return request_status::unavailable;
    }
    if (phase != handoff_phase::idle) {
        return request_status::unavailable;
    }
    if (slot.next_request_sequence == 0U || slot.next_request_sequence == UINT64_MAX) {
        fail_worker(slot, false);
        return request_status::failed;
    }
    auto expected = handoff_phase::idle;
    if (!slot.handoff.compare_exchange_strong(expected,
            handoff_phase::writing_request,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return request_status::unavailable;
    }
    slot.request = render_request{.slot_generation = slot.generation,
        .sequence = slot.next_request_sequence++,
        .audio = audio,
        .frame_bound = frames};
    slot.handoff.store(handoff_phase::requested, std::memory_order_release);
    wake_control(slot);
    return request_status::sent;
}

[[nodiscard]] bool request_render_and_submit(native_port& port,
    audio_slot& slot,
    const std::atomic<bool>& accepting,
    const std::uint32_t capacity,
    const std::uint32_t frames,
    const output::audio_instance audio,
    stream_schedule& schedule) noexcept {
    const auto request = begin_render_request(slot, audio, frames);
    if (request == request_status::failed) {
        return false;
    }
    if (request == request_status::unavailable) {
        submit_silence_or_fail(port, slot, capacity);
        return !slot.transport_closed.load(std::memory_order_acquire);
    }

    auto offer = await_offer(port, slot, accepting, audio);
    if (offer.pending_buffer_event) {
        schedule.last_buffer_event = clock_type::now();
    }
    if (offer.native_failure) {
        complete_handoff(slot);
        fail_worker(slot, true, latest_native_error(slot));
        return false;
    }
    if (offer.admitted.has_value()) {
        auto admitted = std::move(*offer.admitted);
        offer.admitted.reset();
        if (!submit_render(port,
                slot,
                accepting,
                render_frame_limits{.capacity = capacity, .requested_frames = frames},
                std::move(admitted))) {
            complete_handoff(slot);
            fail_worker(slot, true, latest_native_error(slot));
            return false;
        }
        complete_handoff(slot);
        return true;
    }

    complete_handoff(slot);
    if (!accepting.load(std::memory_order_seq_cst) ||
        slot.transport_closed.load(std::memory_order_acquire)) {
        slot.transport_closed.store(true, std::memory_order_release);
        wake_control(slot);
        return false;
    }
    submit_silence_or_fail(port, slot, capacity);
    return !slot.transport_closed.load(std::memory_order_acquire);
}

[[nodiscard]] bool process_buffer_event(native_port& port,
    audio_slot& slot,
    const std::atomic<bool>& accepting,
    const std::uint32_t capacity,
    stream_schedule& schedule) noexcept {
    const auto available = frame_availability(port, slot, capacity);
    if (!available.has_value()) {
        fail_worker(slot, true, latest_native_error(slot));
        return false;
    }
    if (*available == 0U) {
        return true;
    }
    const auto lifecycle = slot.lifecycle.load(std::memory_order_acquire);
    if (lifecycle != slot_lifecycle::live || !slot.ready.load(std::memory_order_acquire) ||
        !slot.audio.has_value()) {
        submit_silence_or_fail(port, slot, capacity);
        return !slot.transport_closed.load(std::memory_order_acquire);
    }
    return request_render_and_submit(port,
        slot,
        accepting,
        capacity,
        *available,
        *slot.audio,
        schedule);
}

} // namespace

void audio_service::implementation::run_native_stream(audio_slot& slot,
    native_port& port,
    const std::uint32_t buffer_size,
    const native_resource_key& resource,
    const std::uint64_t generation) noexcept {
    stream_schedule schedule{};
    while (!slot.transport_closed.load(std::memory_order_acquire)) {
        if (!accepting.load(std::memory_order_seq_cst)) {
            slot.transport_closed.store(true, std::memory_order_seq_cst);
            wake_control(slot);
            break;
        }
        if (!maintain_stream_identity(port, runtime, slot, resource, generation, schedule)) {
            break;
        }
        if (!wait_for_stream_buffer(port, slot, schedule)) {
            continue;
        }
        if (!accepting.load(std::memory_order_seq_cst) ||
            slot.transport_closed.load(std::memory_order_seq_cst)) {
            slot.transport_closed.store(true, std::memory_order_release);
            wake_control(slot);
            break;
        }
        if (!process_buffer_event(port, slot, accepting, buffer_size, schedule)) {
            break;
        }
    }
}
} // namespace inputnext::audio_detail
