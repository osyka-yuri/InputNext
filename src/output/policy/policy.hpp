#pragma once

#include <inputnext/output.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::output::policy_detail {

using clock = std::chrono::steady_clock;
using deadline = clock::time_point;
using optional_deadline = std::optional<deadline>;
// 32 public and 32 internal contexts can each contribute to four channels.
inline constexpr std::size_t maximum_candidate_count = 256U;

// Each view represents a present intent, including OFF and zero-valued payloads.
// Winner indices refer only to the supplied span and are consumed in the current operation.

struct candidate_view final {
    channel source_channel{};
    internal_priority priority{internal_priority::lowest};
    std::uint64_t accepted_commit_order{};
    optional_deadline expires_at;
};

using winner_indices = std::array<std::optional<std::size_t>, channel_count>;

[[nodiscard]] bool is_valid_internal_priority(internal_priority value) noexcept;
[[nodiscard]] bool is_valid_ttl_permission(ttl_permission value) noexcept;
[[nodiscard]] bool is_valid_foreground_policy(foreground_policy value) noexcept;
[[nodiscard]] result<void> validate_internal_context_options(
    const internal_context_options& options) noexcept;
[[nodiscard]] result<void> validate_values(const channel_values& values) noexcept;
[[nodiscard]] result<void> validate_submit_request(const submit_request& request) noexcept;
[[nodiscard]] result<void> validate_internal_submit_request(const submit_request& request,
    ttl_permission permission) noexcept;
[[nodiscard]] result<void> validate_clear_request(const clear_request& request) noexcept;
[[nodiscard]] result<optional_deadline> make_deadline(deadline entry_time,
    std::chrono::milliseconds ttl,
    ttl_permission permission) noexcept;
[[nodiscard]] bool is_expired(optional_deadline expiry, deadline now) noexcept;
[[nodiscard]] result<winner_indices> select_winners(std::span<const candidate_view> candidates,
    deadline now) noexcept;

} // namespace inputnext::output::policy_detail
