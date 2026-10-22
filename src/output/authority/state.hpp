#pragma once

#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace inputnext::output {

namespace detail {

[[nodiscard]] bool foreground_pair_qualifies(std::optional<focus::foreground_epoch> before,
    std::optional<focus::foreground_epoch> after,
    std::span<const std::optional<focus::foreground_epoch>> opening_epochs) noexcept;

} // namespace detail

struct authority::implementation final {
    enum class phase : std::uint8_t { free, open, closed_retained };

    struct channel_state final {
        bool active{};
        policy_detail::optional_deadline expires_at;
        std::uint64_t revision{};
        std::uint64_t accepted_sequence{};
        std::uint64_t accepted_commit_order{};
    };

    struct record final {
        phase state{phase::free};
        context_identity identity{};
        internal_priority priority{internal_priority::game_semantic};
        ttl_permission ttl{ttl_permission::finite_only};
        foreground_policy foreground{foreground_policy::foreground_bound};
        std::optional<focus::foreground_epoch> opening_epoch;
        std::uint64_t accepted_sequence{};
        channel_values accepted_values{};
        std::array<channel_state, channel_count> channels{};
        std::optional<close_receipt> receipt;

        void reset() noexcept { *this = record{}; }
    };

    implementation(runtime_id runtime_value,
        device_registry& registry_value,
        focus::observer& focus_value,
        authority_seed seed_value,
        authority_incarnation incarnation_value);

    [[nodiscard]] policy_detail::deadline now() const noexcept;
    [[nodiscard]] policy_detail::deadline effective_time(
        policy_detail::deadline lower_bound = policy_detail::deadline::min()) noexcept;
    [[nodiscard]] std::size_t open_count() const noexcept;
    [[nodiscard]] std::size_t active_finite_count() const noexcept;
    [[nodiscard]] std::size_t reservation_count() const noexcept;
    [[nodiscard]] bool valid_context_identity(context_identity identity,
        owner_class expected_class) const noexcept;
    [[nodiscard]] static bool matches_grant(const record& entry,
        const internal_context_options& options) noexcept;
    [[nodiscard]] bool existing_internal_grant_conflicts(connection_target target,
        const internal_context_options& options) const noexcept;
    [[nodiscard]] record* find_open(context_identity identity) noexcept;
    [[nodiscard]] record* find_retained(context_identity identity) noexcept;
    [[nodiscard]] record* find_free(owner_class kind) noexcept;
    [[nodiscard]] bool ordinary_headroom(std::size_t reservations_after) const noexcept;
    [[nodiscard]] result<std::uint64_t> take_context_id() noexcept;
    [[nodiscard]] std::optional<focus::foreground_epoch> fresh_epoch() const;
    [[nodiscard]] inputnext::result<read_pin> capture(connection_target target) const;
    [[nodiscard]] std::size_t retire(connection_target target) noexcept;
    void advance_cleanup_version() noexcept;
    static void
    install_close_receipt(record& entry, std::uint64_t close_version, close_reason reason) noexcept;
    [[nodiscard]] result<close_receipt> close_with_retry(context_identity identity,
        owner_class expected_class,
        std::uint64_t close_request_id,
        const internal_context_options* options = nullptr);
    [[nodiscard]] std::size_t expire_target(connection_target target,
        policy_detail::deadline evaluation_time) noexcept;
    [[nodiscard]] std::size_t close_bound_mismatches(connection_target target,
        std::optional<focus::foreground_epoch> observed,
        close_reason reason) noexcept;
    [[nodiscard]] bool close_record(record& entry, close_reason reason) noexcept;
    [[nodiscard]] result<void> qualify_mutation(record& entry,
        connection_target target,
        std::optional<focus::foreground_epoch> expected);
    [[nodiscard]] result<mutation_receipt> commit_submit_locked(record& entry,
        const submit_request& request,
        policy_detail::optional_deadline deadline,
        policy_detail::deadline entry_time) noexcept;
    [[nodiscard]] result<logical_snapshot> compose_snapshot(connection_target target,
        policy_detail::deadline evaluation_time,
        bool permit_bound,
        std::optional<focus::foreground_epoch> epoch) noexcept;
    [[nodiscard]] result<logical_snapshot> qualify_snapshot_locked(connection_target target);
    [[nodiscard]] result<logical_snapshot> finish_snapshot_locked(connection_target target,
        policy_detail::deadline evaluation_time,
        std::optional<focus::foreground_epoch> before,
        std::optional<focus::foreground_epoch> after,
        inputnext::read_pin first_pin) noexcept;

    const runtime_id runtime;
    const authority_incarnation incarnation;
    device_registry& registry;
    focus::observer& focus_observer;
    mutable std::mutex gate;
    std::array<record, 64U> records{};
    std::uint64_t next_context_id{};
    std::uint64_t next_execution_id{1U};
    std::optional<execution_owner> attached_execution;
    std::uint64_t logical_version{};
    policy_detail::deadline last_evaluation_time;
    const std::atomic<std::int64_t>* test_clock_ticks{};
};

} // namespace inputnext::output
