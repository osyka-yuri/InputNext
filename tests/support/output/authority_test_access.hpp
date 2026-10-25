#pragma once

#include "output/authority/incarnation.hpp"
#include "output/authority/state.hpp"
#include "output/playback/body_playback_bank.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::output::detail {

class authority_test_access final {
public:
    struct record_observation final {
        context_identity identity{};
        bool open{};
        std::uint64_t accepted_sequence{};
        channel_revisions revisions{};
        std::array<bool, channel_count> active{};
        std::optional<body_snapshot> body;
        std::optional<body_playback_identity> playback;
        std::optional<close_receipt> receipt;
        std::uint64_t logical_version{};
    };

    [[nodiscard]] static authority make_with_generation_failure(runtime_id runtime,
        device_registry& registry,
        focus::observer& focus_observer,
        authority_seed seed = {}) {
        return authority{runtime,
            registry,
            focus_observer,
            seed,
            incarnation_creation::force_failure_for_test};
    }

    [[nodiscard]] static authority_incarnation incarnation(const authority& value) noexcept {
        return value.implementation_->incarnation;
    }

    [[nodiscard]] static std::uint64_t logical_version(authority& value) {
        auto lock = lock_gate(value);
        return value.implementation_->logical_version;
    }

    [[nodiscard]] static std::optional<record_observation> observe_record(authority& value,
        context_identity identity) {
        auto lock = lock_gate(value);
        auto* entry = value.implementation_->find_open(identity);
        const bool open = entry != nullptr;
        if (entry == nullptr) {
            entry = value.implementation_->find_retained(identity);
        }
        if (entry == nullptr) {
            return std::nullopt;
        }
        record_observation observation;
        observation.identity = entry->identity;
        observation.open = open;
        observation.accepted_sequence = entry->accepted_sequence;
        observation.receipt = entry->receipt;
        observation.body = entry->body;
        observation.playback = entry->playback;
        observation.logical_version = value.implementation_->logical_version;
        for (std::size_t index = 0U; index < channel_count; ++index) {
            observation.revisions[index] = entry->channels[index].revision;
            observation.active[index] = entry->channels[index].active;
        }
        return observation;
    }

    [[nodiscard]] static std::unique_lock<std::mutex> lock_gate(authority& value) {
        return std::unique_lock{value.implementation_->gate};
    }

    static void use_clock(authority& value, const std::atomic<std::int64_t>& ticks) noexcept {
        value.implementation_->test_clock_ticks = &ticks;
    }

    // Call only before preparing samples or submitting any body playback command; this replaces
    // the authority's empty preparation store with a deterministic controlled memory port.
    static void use_pcm_preparer(authority& value, body_pcm_preparer preparer) {
        auto lock = lock_gate(value);
        value.implementation_->pcm_preparer = std::move(preparer);
    }

    [[nodiscard]] static result<body_receipt> commit_body_locked(authority& value,
        context_identity identity,
        const body_request& request,
        std::chrono::steady_clock::time_point entry_time) {
        body_playback_retirement_batch displaced;
        auto lock = lock_gate(value);
        auto* const entry = value.implementation_->find_open(identity);
        if (entry == nullptr) {
            return std::unexpected{error::stale_context};
        }
        const auto expires =
            policy_detail::make_deadline(entry_time, request.ttl, ttl_permission::finite_only);
        if (!expires.has_value()) {
            return std::unexpected{expires.error()};
        }
        if (!expires->has_value()) {
            return std::unexpected{error::deadline_overflow};
        }
        auto committed = value.implementation_->commit_body_playback_locked(*entry,
            request,
            {.entered_at = entry_time, .expires_at = **expires},
            body_pcm_command::keep,
            prepared_body_pcm{},
            displaced);
        if (!committed.has_value()) {
            return std::unexpected{committed.error()};
        }
        return committed->body;
    }

    static void set_logical_version(authority& value, std::uint64_t version) {
        auto lock = lock_gate(value);
        value.implementation_->logical_version = version;
    }

    static void set_next_execution_id(authority& value, std::uint64_t next_id) {
        auto lock = lock_gate(value);
        value.implementation_->next_execution_id = next_id;
    }

    static void set_next_body_lease_id(authority& value, std::uint64_t next_id) {
        auto lock = lock_gate(value);
        value.implementation_->next_body_lease_id = next_id;
    }

    static void set_next_body_playback_revision(authority& value, std::uint64_t next_id) {
        auto lock = lock_gate(value);
        value.implementation_->next_body_playback_revision = next_id;
    }

    [[nodiscard]] static std::uint64_t next_body_lease_id(authority& value) {
        auto lock = lock_gate(value);
        return value.implementation_->next_body_lease_id;
    }

    static void set_next_audio_instance_id(authority& value, std::uint64_t next_id) {
        auto lock = lock_gate(value);
        value.implementation_->next_audio_instance_id = next_id;
    }

    [[nodiscard]] static std::uint64_t next_execution_id(authority& value) {
        auto lock = lock_gate(value);
        return value.implementation_->next_execution_id;
    }

    [[nodiscard]] static std::optional<execution_owner> attached_execution(authority& value) {
        auto lock = lock_gate(value);
        return value.implementation_->attached_execution;
    }

    [[nodiscard]] static bool set_channel_revision(authority& value,
        context_identity identity,
        channel selected,
        std::uint64_t revision) {
        auto lock = lock_gate(value);
        auto* const entry = value.implementation_->find_open(identity);
        if (entry == nullptr) {
            return false;
        }
        entry->channels[static_cast<std::size_t>(selected)].revision = revision;
        return true;
    }

    [[nodiscard]] static bool set_opening_epoch(authority& value,
        context_identity identity,
        std::optional<focus::foreground_epoch> epoch) {
        auto lock = lock_gate(value);
        auto* const entry = value.implementation_->find_open(identity);
        if (entry == nullptr) {
            return false;
        }
        entry->opening_epoch = epoch;
        return true;
    }

    [[nodiscard]] static result<logical_snapshot> finish_snapshot_locked(authority& value,
        connection_target target,
        std::chrono::steady_clock::time_point evaluation_time,
        std::optional<focus::foreground_epoch> before,
        std::optional<focus::foreground_epoch> after,
        read_pin first_pin) noexcept {
        // The caller holds the authority gate, matching the production continuation boundary.
        return value.implementation_->finish_snapshot_locked(target,
            evaluation_time,
            before,
            after,
            std::move(first_pin));
    }

    [[nodiscard]] static result<mutation_receipt> commit_submit_locked(authority& value,
        context_identity identity,
        const submit_request& request,
        policy_detail::optional_deadline deadline,
        policy_detail::deadline entry_time) noexcept {
        auto* const entry = value.implementation_->find_open(identity);
        if (entry == nullptr) {
            return std::unexpected{error::stale_context};
        }
        return value.implementation_->commit_submit_locked(*entry, request, deadline, entry_time);
    }
};

} // namespace inputnext::output::detail
