#include "detail.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/device_metadata.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace inputnext::audio_detail {
namespace {

[[nodiscard]] std::uint64_t
saturating_sum(const std::uint64_t left, const std::uint64_t right, bool& saturated) noexcept {
    if (right > UINT64_MAX - left) {
        saturated = true;
        return UINT64_MAX;
    }
    return left + right;
}

[[nodiscard]] audio_target_progress make_target_progress(const audio_slot& slot,
    const slot_lifecycle lifecycle) noexcept {
    audio_target_progress target{};
    target.target = slot.target;
    if (slot.backend.valid()) {
        target.backend = slot.backend;
    }
    target.audio = slot.audio;
    target.last_admitted_route = slot.last_admitted_route;
    target.last_admitted_playback = slot.last_admitted_playback;
    target.last_route_error = slot.last_route_error;
    if (slot.has_metadata_error.load(std::memory_order_relaxed)) {
        target.last_metadata_error =
            static_cast<metadata_error>(slot.last_metadata_error.load(std::memory_order_relaxed));
    }
    switch (lifecycle) {
    case slot_lifecycle::opening:
        target.phase = slot.setup.load(std::memory_order_acquire) == setup_phase::candidate_ready
                           ? audio_target_phase::waiting_for_reservation
                           : audio_target_phase::opening;
        break;
    case slot_lifecycle::prepared:
        target.phase = audio_target_phase::opening;
        break;
    case slot_lifecycle::live:
        target.phase = audio_target_phase::live;
        break;
    case slot_lifecycle::revoking:
        target.phase = audio_target_phase::revoking;
        break;
    case slot_lifecycle::draining:
        target.phase = audio_target_phase::draining;
        break;
    case slot_lifecycle::retained:
        target.phase = audio_target_phase::retained;
        break;
    case slot_lifecycle::vacant:
        break;
    }
    target.transport_open = !slot.transport_closed.load(std::memory_order_acquire);
    target.native_setup_started = slot.native_setup_started.load(std::memory_order_relaxed);
    target.native_stream_started = slot.native_stream_started.load(std::memory_order_relaxed);
    target.ready = slot.ready.load(std::memory_order_acquire);
    target.mmcss_registered = slot.mmcss_registered.load(std::memory_order_relaxed);
    if (slot.has_native_error.load(std::memory_order_acquire)) {
        target.last_native_error = latest_native_error(slot);
        target.last_native_operation = target.last_native_error->operation;
    }
    target.admitted_blocks = slot.admitted_blocks.value.load(std::memory_order_relaxed);
    target.source_frames_consumed =
        slot.source_frames_consumed.value.load(std::memory_order_relaxed);
    target.accepted_pcm_buffers = slot.accepted_pcm_buffers.value.load(std::memory_order_relaxed);
    target.accepted_pcm_frames = slot.accepted_pcm_frames.value.load(std::memory_order_relaxed);
    target.accepted_silent_buffers =
        slot.accepted_silent_buffers.value.load(std::memory_order_relaxed);
    target.counters_saturated = slot.counters_saturated.load(std::memory_order_relaxed);
    return target;
}

void add_target_totals(audio_service_progress& progress,
    const audio_target_progress& target) noexcept {
    progress.admitted_blocks = saturating_sum(progress.admitted_blocks,
        target.admitted_blocks,
        progress.counters_saturated);
    progress.source_frames_consumed = saturating_sum(progress.source_frames_consumed,
        target.source_frames_consumed,
        progress.counters_saturated);
    progress.accepted_pcm_buffers = saturating_sum(progress.accepted_pcm_buffers,
        target.accepted_pcm_buffers,
        progress.counters_saturated);
    progress.accepted_pcm_frames = saturating_sum(progress.accepted_pcm_frames,
        target.accepted_pcm_frames,
        progress.counters_saturated);
    progress.accepted_silent_buffers = saturating_sum(progress.accepted_silent_buffers,
        target.accepted_silent_buffers,
        progress.counters_saturated);
    progress.counters_saturated = progress.counters_saturated || target.counters_saturated;
}

void add_slot_progress(audio_service_progress& result, const audio_slot& slot) noexcept {
    const auto lifecycle = slot.lifecycle.load(std::memory_order_acquire);
    if (lifecycle == slot_lifecycle::vacant) {
        return;
    }
    ++result.resident_slots;
    result.live_slots += lifecycle == slot_lifecycle::live ? 1U : 0U;
    result.retained_slots += lifecycle == slot_lifecycle::retained ? 1U : 0U;
    if (result.target_count == result.targets.size()) {
        return;
    }
    const auto target = make_target_progress(slot, lifecycle);
    add_target_totals(result, target);
    result.targets[result.target_count++] = target;
}

} // namespace

audio_service_progress audio_service::implementation::snapshot_progress() const noexcept {
    audio_service_progress result{};
    result.accepting = accepting.load(std::memory_order_acquire);
    result.stopping = stopping.load(std::memory_order_acquire);
    result.desired_targets = active_inventory.size;
    result.admitted_blocks = retired_admitted_blocks;
    result.source_frames_consumed = retired_source_frames_consumed;
    result.accepted_pcm_buffers = retired_accepted_pcm_buffers;
    result.accepted_pcm_frames = retired_accepted_pcm_frames;
    result.accepted_silent_buffers = retired_accepted_silent_buffers;
    result.counters_saturated = retired_counters_saturated;
    for (const auto& slot : slots) {
        add_slot_progress(result, slot);
    }
    for (const auto& retry : retries) {
        if (retry.occupied && retry.last_start_failure.has_value() &&
            result.start_failure_count < result.start_failures.size()) {
            result.start_failures[result.start_failure_count++] = *retry.last_start_failure;
        }
    }
    return result;
}

void audio_service::implementation::accumulate_retired_telemetry(audio_slot& slot) noexcept {
    if (slot.telemetry_collected) {
        return;
    }
    slot.telemetry_collected = true;
    retired_admitted_blocks = saturating_sum(retired_admitted_blocks,
        slot.admitted_blocks.value.load(std::memory_order_relaxed),
        retired_counters_saturated);
    retired_source_frames_consumed = saturating_sum(retired_source_frames_consumed,
        slot.source_frames_consumed.value.load(std::memory_order_relaxed),
        retired_counters_saturated);
    retired_accepted_pcm_buffers = saturating_sum(retired_accepted_pcm_buffers,
        slot.accepted_pcm_buffers.value.load(std::memory_order_relaxed),
        retired_counters_saturated);
    retired_accepted_pcm_frames = saturating_sum(retired_accepted_pcm_frames,
        slot.accepted_pcm_frames.value.load(std::memory_order_relaxed),
        retired_counters_saturated);
    retired_accepted_silent_buffers = saturating_sum(retired_accepted_silent_buffers,
        slot.accepted_silent_buffers.value.load(std::memory_order_relaxed),
        retired_counters_saturated);
    retired_counters_saturated =
        retired_counters_saturated || slot.counters_saturated.load(std::memory_order_relaxed);
}

void audio_service::implementation::publish_progress() noexcept {
    const auto snapshot = snapshot_progress();
    std::scoped_lock lock{publication_mutex};
    published_progress = snapshot;
}

} // namespace inputnext::audio_detail
