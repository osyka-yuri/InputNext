#include "execution.hpp"

#include "admitted_body_render.hpp"
#include "output/playback/body_playback_bank.hpp"
#include "output/policy/body.hpp"
#include "output/projection/represented.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::output::detail {
namespace {

[[nodiscard]] execution_error map_snapshot_error(error value) noexcept {
    switch (value) {
    case error::not_connected:
        return execution_error::not_connected;
    case error::invalid_argument:
        return execution_error::invalid_argument;
    default:
        return execution_error::resources;
    }
}

[[nodiscard]] bool valid_expected_hd_route(const execution_owner owner,
    const runtime_id runtime,
    const body_route_intent& expected) noexcept {
    return expected.identity.valid() && expected.identity.context.incarnation == owner.authority &&
           expected.identity.context.target.runtime == runtime && expected.revision != 0U &&
           expected.route == body_actuator_route::hd &&
           expected.fallback == body_fallback_reason::none;
}

struct body_render_claim final {
    body_playback_identity playback;
    body_render_permit permit;
};

template <typename State>
void retire_playback_for_route_change_locked(State& state,
    const body_route_intent& expected,
    const std::optional<body_route_intent>& current) noexcept {
    if (!current.has_value() || current->identity != expected.identity ||
        current->route == body_actuator_route::hd) {
        return;
    }
    if (auto* const entry = state.find_open(expected.identity.context); entry != nullptr) {
        state.mark_started_playback_retired_locked(*entry);
    }
}

[[nodiscard]] bool valid_body_render_request(const execution_owner expected_owner,
    const backend_handle expected_backend,
    const body_route_ack& acknowledgement,
    const std::optional<body_playback_identity> expected_playback,
    const audio_instance expected_audio,
    std::atomic<bool>* const engine_busy,
    const std::size_t requested_frames) noexcept {
    return expected_owner.valid() && expected_backend.valid() && engine_busy != nullptr &&
           requested_frames != 0U &&
           requested_frames <= std::numeric_limits<std::size_t>::max() / 4U &&
           acknowledgement.backend.valid() && acknowledgement.backend == expected_backend &&
           acknowledgement.backend.incarnation.owner == expected_owner &&
           acknowledgement.intent.route == body_actuator_route::hd &&
           acknowledgement.intent.fallback == body_fallback_reason::none &&
           acknowledgement.publication.valid() && acknowledgement.attempt.valid() &&
           (!expected_playback.has_value() ||
               (expected_playback->valid() &&
                   acknowledgement.intent.identity == expected_playback->lease &&
                   expected_playback->lease.audio == expected_audio));
}

template <typename State>
[[nodiscard]] execution_result<std::optional<body_render_claim>> qualify_and_claim_body_render(
    State& state,
    const execution_owner expected_owner,
    const backend_handle expected_backend,
    const body_route_ack& acknowledgement,
    const std::optional<body_playback_identity> expected_playback,
    const audio_instance expected_audio,
    std::atomic<bool>* const engine_busy,
    const std::size_t requested_frames) noexcept {
    if (!valid_body_render_request(expected_owner,
            expected_backend,
            acknowledgement,
            expected_playback,
            expected_audio,
            engine_busy,
            requested_frames)) {
        return std::unexpected{execution_error::invalid_argument};
    }

    std::scoped_lock lock{state.gate};
    if (!state.attached_execution.has_value() || *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!valid_expected_hd_route(expected_owner, state.runtime, acknowledgement.intent) ||
        acknowledgement.backend.target != expected_audio.target ||
        expected_audio.authority != expected_owner.authority) {
        return std::optional<body_render_claim>{};
    }

    auto* const entry = state.find_open(acknowledgement.intent.identity.context);
    if (entry == nullptr || !entry->body.has_value() || !entry->playback.has_value() ||
        entry->body->intent != acknowledgement.intent ||
        entry->body->intent.identity != acknowledgement.intent.identity) {
        return std::optional<body_render_claim>{};
    }

    const auto playback = *entry->playback;
    if (!playback.valid() || playback.lease != acknowledgement.intent.identity ||
        playback.lease.audio != expected_audio ||
        (expected_playback.has_value() && playback != *expected_playback)) {
        return std::optional<body_render_claim>{};
    }

    const auto current = state.qualify_snapshot_locked(expected_backend.target);
    if (!current.has_value()) {
        if (current.error() == error::not_connected) {
            return std::optional<body_render_claim>{};
        }
        return std::unexpected{map_snapshot_error(current.error())};
    }
    const auto represented = project_representation(*current, rumble_mask, body_route_support::hd);
    if (!represented.has_value()) {
        return std::unexpected{represented.error()};
    }
    const auto route = represented_body_route(*represented);
    if (!route.has_value() || *route != acknowledgement.intent) {
        retire_playback_for_route_change_locked(state, acknowledgement.intent, route);
        return std::optional<body_render_claim>{};
    }

    const auto audio = state.audio_facts(expected_audio);
    if (!audio.has_value() || audio->instance != expected_audio || !audio->ready ||
        audio->support != body_route_support::hd) {
        return std::optional<body_render_claim>{};
    }

    bool idle = false;
    if (!engine_busy->compare_exchange_strong(idle,
            true,
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
        return std::unexpected{execution_error::busy};
    }

    auto claimed =
        state.playback_bank.try_claim(playback, expected_backend, engine_busy, requested_frames);
    if (!claimed.has_value()) {
        engine_busy->store(false, std::memory_order_release);
        engine_busy->notify_all();
        switch (claimed.error()) {
        case body_playback_admission_error::invalid_argument:
            return std::unexpected{execution_error::invalid_argument};
        case body_playback_admission_error::busy:
            return std::unexpected{execution_error::busy};
        case body_playback_admission_error::stale_backend:
            return std::unexpected{execution_error::stale_backend};
        }
        return std::unexpected{execution_error::resources};
    }
    if (!claimed->has_value()) {
        engine_busy->store(false, std::memory_order_release);
        engine_busy->notify_all();
        return std::optional<body_render_claim>{};
    }
    return std::optional<body_render_claim>{body_render_claim{playback, std::move(**claimed)}};
}

} // namespace

execution_result<execution_owner> authority_execution_access::attach(authority& owner) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (state.attached_execution.has_value()) {
        return std::unexpected{execution_error::already_attached};
    }
    if (state.next_execution_id == 0U) {
        return std::unexpected{execution_error::identity_exhausted};
    }

    const execution_owner attached{.authority = state.incarnation,
        .id = execution_id{state.next_execution_id}};
    if (state.next_execution_id == std::numeric_limits<std::uint64_t>::max()) {
        state.next_execution_id = 0U;
    } else {
        ++state.next_execution_id;
    }
    state.attached_execution = attached;
    return attached;
}

execution_result<void> authority_execution_access::detach(authority& owner,
    const execution_owner expected_owner) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!state.attached_execution.has_value()) {
        return std::unexpected{execution_error::not_running};
    }
    if (!expected_owner.valid() || *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    state.attached_execution.reset();
    return {};
}

execution_result<void> authority_execution_access::check_target(authority& owner,
    const execution_owner expected_owner,
    const connection_target target) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!target.valid()) {
        return std::unexpected{execution_error::invalid_argument};
    }
    if (target.runtime != state.runtime) {
        return std::unexpected{execution_error::not_connected};
    }

    auto pin = state.capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            static_cast<void>(state.retire(target));
            return std::unexpected{execution_error::not_connected};
        }
        return std::unexpected{execution_error::resources};
    }
    pin->release();
    return {};
}

execution_result<bool> authority_execution_access::qualifies(authority& owner,
    const execution_owner expected_owner,
    const represented_projection& expected) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;

    // Reject a foreign or stale execution owner before evaluating time or maintaining records.
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!expected.target.valid() || expected.target.runtime != state.runtime ||
        expected.incarnation != expected_owner.authority || expected.channels == 0U ||
        (expected.channels & ~supported_channel_mask) != 0U) {
        return std::unexpected{execution_error::invalid_argument};
    }

    auto current = state.qualify_snapshot_locked(expected.target);
    if (!current.has_value()) {
        return std::unexpected{map_snapshot_error(current.error())};
    }
    auto projected = project_representation(*current, expected.channels, expected.route_support);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    return *projected == expected;
}

execution_result<bool> authority_execution_access::qualifies_route(authority& owner,
    const execution_owner expected_owner,
    const body_route_intent& expected) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!valid_expected_hd_route(expected_owner, state.runtime, expected)) {
        return std::unexpected{execution_error::invalid_argument};
    }

    const auto target = expected.identity.context.target;
    const auto current = state.qualify_snapshot_locked(target);
    if (!current.has_value()) {
        return std::unexpected{map_snapshot_error(current.error())};
    }
    const auto projected = project_representation(*current, rumble_mask, body_route_support::hd);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    const auto current_route = represented_body_route(*projected);
    if (!current_route.has_value() || *current_route != expected) {
        retire_playback_for_route_change_locked(state, expected, current_route);
        return false;
    }
    return true;
}

execution_result<completion_disposition> authority_execution_access::complete_route(
    authority& owner,
    const execution_owner expected_owner,
    const body_route_intent& expected,
    const native_status status) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!valid_expected_hd_route(expected_owner, state.runtime, expected)) {
        return std::unexpected{execution_error::invalid_argument};
    }

    const auto target = expected.identity.context.target;
    const auto current = state.qualify_snapshot_locked(target);
    if (!current.has_value()) {
        return std::unexpected{map_snapshot_error(current.error())};
    }
    const auto projected = project_representation(*current, rumble_mask, body_route_support::hd);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    const auto current_route = represented_body_route(*projected);
    if (!current_route.has_value() || *current_route != expected) {
        retire_playback_for_route_change_locked(state, expected, current_route);
        return completion_disposition::stale;
    }
    if (status == native_status::accepted) {
        return completion_disposition::current;
    }

    auto* const record = state.find_open(expected.identity.context);
    if (record == nullptr || !record->body.has_value() || record->body->intent != expected) {
        return completion_disposition::stale;
    }
    if (!state.ordinary_headroom(state.reservation_count() - 1U)) {
        return std::unexpected{execution_error::resources};
    }

    const auto version = state.logical_version + 1U;
    state.mark_started_playback_retired_locked(*record);
    record->body->intent = body_policy::fallback_route(record->body->intent,
        body_fallback_reason::output_failed,
        version);
    state.logical_version = version;
    return completion_disposition::fallback_committed;
}

execution_result<std::optional<body_render_permit>> authority_execution_access::admit_body_render(
    authority& owner,
    const execution_owner expected_owner,
    const body_route_ack& acknowledgement,
    const body_playback_identity expected_playback,
    std::atomic<bool>* const engine_busy,
    const std::size_t requested_frames) noexcept {
    auto& state = *owner.implementation_;
    auto claimed = qualify_and_claim_body_render(state,
        expected_owner,
        acknowledgement.backend,
        acknowledgement,
        expected_playback,
        expected_playback.lease.audio,
        engine_busy,
        requested_frames);
    if (!claimed.has_value()) {
        return std::unexpected{claimed.error()};
    }
    if (!claimed->has_value()) {
        return std::optional<body_render_permit>{};
    }
    return std::optional<body_render_permit>{std::move((**claimed).permit)};
}

execution_result<std::optional<admitted_body_render>>
authority_execution_access::admit_current_body_render(authority& owner,
    const execution_owner expected_owner,
    const backend_handle expected_backend,
    const body_route_ack& acknowledgement,
    const audio_instance expected_audio,
    std::atomic<bool>* const engine_busy,
    const std::size_t requested_frames) noexcept {
    auto& state = *owner.implementation_;
    auto claimed = qualify_and_claim_body_render(state,
        expected_owner,
        expected_backend,
        acknowledgement,
        std::nullopt,
        expected_audio,
        engine_busy,
        requested_frames);
    if (!claimed.has_value()) {
        return std::unexpected{claimed.error()};
    }
    if (!claimed->has_value()) {
        return std::optional<admitted_body_render>{};
    }
    auto current = std::move(**claimed);
    return std::optional<admitted_body_render>{admitted_body_render{.route = acknowledgement,
        .playback = current.playback,
        .permit = std::move(current.permit)}};
}

void authority_execution_access::retire_body_backend(authority& owner,
    const backend_handle backend) noexcept {
    auto& state = *owner.implementation_;
    std::scoped_lock lock{state.gate};
    static_cast<void>(state.playback_bank.retire_backend(backend));
}

} // namespace inputnext::output::detail
