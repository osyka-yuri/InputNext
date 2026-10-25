#include "body_playback_bank.hpp"

#include <inputnext/device_identity.hpp>

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace inputnext::output::detail {

void body_playback_retirement_batch::append(prepared_body_pcm&& samples) noexcept {
    entries_[count_++] = std::move(samples);
}

body_playback_voice_slot& body_playback_bank::at(const body_playback_slot_index index) noexcept {
    return slots_[index.value];
}

const body_playback_voice_slot& body_playback_bank::at(
    const body_playback_slot_index index) const noexcept {
    return slots_[index.value];
}

bool body_playback_bank::same_target(const body_playback_voice_slot& slot,
    const connection_target target) noexcept {
    return slot.samples.valid() && slot.identity.lease.context.target == target;
}

std::optional<body_playback_slot_index> body_playback_bank::find_current(
    const body_lease_identity identity) const noexcept {
    if (!identity.valid()) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < slots_.size(); ++index) {
        const auto& slot = slots_[index];
        if (slot.samples.valid() && !slot.retired && slot.identity.lease == identity) {
            return body_playback_slot_index{static_cast<std::uint8_t>(index)};
        }
    }
    return std::nullopt;
}

std::optional<body_playback_slot_selection> body_playback_bank::select_install_slot(
    const connection_target target,
    const std::optional<body_playback_slot_index> current) const noexcept {
    if (!target.valid()) {
        return std::nullopt;
    }

    for (std::size_t index = 0U; index < slots_.size(); ++index) {
        const auto& slot = slots_[index];
        if (!slot.samples.valid() && !slot.busy.load(std::memory_order_acquire)) {
            return body_playback_slot_selection{
                .slot = body_playback_slot_index{static_cast<std::uint8_t>(index)},
                .target = target,
                .displaced = std::nullopt,
            };
        }
    }

    if (!current.has_value() || !current->valid()) {
        return std::nullopt;
    }
    const auto& slot = at(*current);
    if (!same_target(slot, target) || slot.retired || slot.busy.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return body_playback_slot_selection{
        .slot = *current,
        .target = target,
        .displaced = slot.identity,
    };
}

bool body_playback_bank::install(const body_playback_slot_selection selection,
    const std::optional<body_playback_identity> previous,
    const body_playback_identity identity,
    prepared_body_pcm&& replacement,
    body_playback_retirement_batch& displaced) noexcept {
    if (!selection.slot.valid() || !selection.target.valid() || !identity.valid() ||
        identity.lease.context.target != selection.target || !replacement.valid()) {
        return false;
    }

    auto& slot = at(selection.slot);
    if (slot.busy.load(std::memory_order_acquire)) {
        return false;
    }

    body_playback_voice_slot* previous_slot{};
    if (previous.has_value()) {
        for (auto& candidate : slots_) {
            if (candidate.samples.valid() && candidate.identity == *previous &&
                !candidate.retired) {
                previous_slot = &candidate;
                break;
            }
        }
        if (previous_slot == nullptr || previous->lease.context.target != selection.target) {
            return false;
        }
    }

    if (selection.displaced.has_value()) {
        if (!previous.has_value() || *previous != *selection.displaced || previous_slot != &slot ||
            !displaced.has_space() || !slot.samples.valid() || slot.retired ||
            slot.identity != *selection.displaced ||
            slot.identity.lease.context.target != selection.target) {
            return false;
        }
        displaced.append(std::move(slot.samples));
    } else if (slot.samples.valid()) {
        return false;
    }

    if (previous_slot != nullptr && previous_slot != &slot) {
        previous_slot->retired = true;
    }

    slot.samples = std::move(replacement);
    slot.identity = identity;
    slot.backend.reset();
    slot.retired = false;
    slot.cursor = 0U;
    slot.completed = false;
    return true;
}

bool body_playback_bank::stop(const body_lease_identity identity) noexcept {
    return retire_lease(identity) != 0U;
}

std::size_t body_playback_bank::retire(const body_playback_identity identity) noexcept {
    if (!identity.valid()) {
        return 0U;
    }
    std::size_t changed{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && slot.identity == identity && !slot.retired) {
            slot.retired = true;
            ++changed;
        }
    }
    return changed;
}

std::size_t body_playback_bank::retire_lease(const body_lease_identity identity) noexcept {
    if (!identity.valid()) {
        return 0U;
    }
    std::size_t changed{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && slot.identity.lease == identity && !slot.retired) {
            slot.retired = true;
            ++changed;
        }
    }
    return changed;
}

std::size_t body_playback_bank::retire_started_lease(const body_lease_identity identity) noexcept {
    if (!identity.valid()) {
        return 0U;
    }
    std::size_t changed{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && slot.identity.lease == identity && slot.backend.has_value() &&
            !slot.retired) {
            slot.retired = true;
            ++changed;
        }
    }
    return changed;
}

std::size_t body_playback_bank::retire_audio(const audio_instance identity) noexcept {
    if (!identity.valid()) {
        return 0U;
    }
    std::size_t changed{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && slot.identity.lease.audio == identity && !slot.retired) {
            slot.retired = true;
            ++changed;
        }
    }
    return changed;
}

std::size_t body_playback_bank::retire_backend(const backend_handle identity) noexcept {
    if (!identity.valid()) {
        return 0U;
    }
    std::size_t changed{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && slot.backend == identity && !slot.retired) {
            slot.retired = true;
            ++changed;
        }
    }
    return changed;
}

std::expected<std::optional<body_render_permit>, body_playback_admission_error>
body_playback_bank::try_claim(const body_playback_identity expected,
    const backend_handle backend,
    std::atomic<bool>* const engine_busy,
    const std::size_t requested_frames) noexcept {
    if (!expected.valid() || !backend.valid() || engine_busy == nullptr ||
        !engine_busy->load(std::memory_order_acquire) || requested_frames == 0U) {
        return std::unexpected{body_playback_admission_error::invalid_argument};
    }

    const auto target = expected.lease.context.target;
    for (const auto& slot : slots_) {
        if (same_target(slot, target) && slot.busy.load(std::memory_order_acquire)) {
            return std::unexpected{body_playback_admission_error::busy};
        }
    }

    body_playback_voice_slot* candidate{};
    for (auto& slot : slots_) {
        if (slot.samples.valid() && !slot.retired && slot.identity == expected) {
            candidate = &slot;
            break;
        }
    }
    if (candidate == nullptr) {
        return std::optional<body_render_permit>{};
    }
    if (candidate->backend.has_value() && *candidate->backend != backend) {
        return std::unexpected{body_playback_admission_error::stale_backend};
    }

    // The authority gate serializes new claims. Read renderer-owned terminal fields only after
    // acquire-observing the idle slot; never inspect them once busy is published.
    if (candidate->busy.load(std::memory_order_acquire)) {
        return std::unexpected{body_playback_admission_error::busy};
    }
    const auto pcm = candidate->samples.view();
    if (candidate->completed || candidate->cursor >= pcm.frame_count) {
        return std::optional<body_render_permit>{};
    }

    bool idle = false;
    if (!candidate->busy.compare_exchange_strong(idle,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return std::unexpected{body_playback_admission_error::busy};
    }
    if (!candidate->backend.has_value()) {
        candidate->backend = backend;
    }
    return std::optional<body_render_permit>{
        body_render_permit{*candidate, *engine_busy, requested_frames}};
}

std::size_t body_playback_bank::collect_nonbusy(
    body_playback_retirement_batch& destination) noexcept {
    std::size_t collected{};
    for (auto& slot : slots_) {
        if (destination.size() == body_playback_voice_capacity) {
            break;
        }
        if (!slot.samples.valid() || slot.busy.load(std::memory_order_acquire)) {
            continue;
        }
        const auto frame_count = slot.samples.view().frame_count;
        const bool completed = slot.completed || slot.cursor >= frame_count;
        if (!slot.retired && !completed) {
            continue;
        }

        destination.append(std::move(slot.samples));
        slot.identity = {};
        slot.backend.reset();
        slot.retired = false;
        slot.cursor = 0U;
        slot.completed = false;
        ++collected;
    }
    return collected;
}

} // namespace inputnext::output::detail
