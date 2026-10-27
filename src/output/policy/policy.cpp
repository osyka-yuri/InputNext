#include "policy.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_submission.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ratio>
#include <span>

namespace inputnext::output::policy_detail {
namespace {

[[nodiscard]] constexpr result<void> invalid_argument() noexcept {
    return std::unexpected{error::invalid_argument};
}

[[nodiscard]] bool valid_channel_mask(const channel_mask value) noexcept {
    return value != 0U && (value & ~supported_channel_mask) == 0U;
}

[[nodiscard]] bool valid_trigger_mode(const trigger_mode value) noexcept {
    switch (value) {
    case trigger_mode::off:
    case trigger_mode::feedback:
    case trigger_mode::weapon:
    case trigger_mode::vibration:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_trigger_layout(const trigger_layout value) noexcept {
    switch (value) {
    case trigger_layout::uniform:
    case trigger_layout::per_zone:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_trigger(const trigger_value& value) noexcept {
    if (!valid_trigger_mode(value.mode) || !valid_trigger_layout(value.layout) ||
        !std::isfinite(value.start) || !std::isfinite(value.end) ||
        !std::isfinite(value.strength) || !std::isfinite(value.frequency) || value.start < 0.0F ||
        value.start > 1.0F || value.end < value.start || value.end > 1.0F ||
        value.strength < 0.0F || value.strength > 1.0F || value.frequency < 0.0F ||
        value.frequency > maximum_trigger_frequency) {
        return false;
    }
    return std::ranges::all_of(value.zones,
        [](const std::uint8_t level) { return level <= maximum_trigger_zone_level; });
}

[[nodiscard]] bool valid_fraction(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] constexpr std::size_t index_for(const channel value) noexcept {
    switch (value) {
    case channel::left_trigger:
        return 0U;
    case channel::right_trigger:
        return 1U;
    case channel::rumble:
        return 2U;
    case channel::leds:
        return 3U;
    }
    return channel_count;
}

[[nodiscard]] bool valid_ttl(const std::chrono::milliseconds ttl,
    const ttl_permission permission) noexcept {
    if (!is_valid_ttl_permission(permission) || ttl.count() < 0 || ttl > maximum_finite_ttl) {
        return false;
    }
    if (ttl.count() == 0) {
        return permission == ttl_permission::finite_or_indefinite;
    }
    return true;
}

} // namespace

bool is_valid_internal_priority(const internal_priority value) noexcept {
    switch (value) {
    case internal_priority::lowest:
    case internal_priority::generic_profile:
    case internal_priority::game_profile:
    case internal_priority::game_semantic:
    case internal_priority::diagnostic:
        return true;
    }
    return false;
}

bool is_valid_ttl_permission(const ttl_permission value) noexcept {
    switch (value) {
    case ttl_permission::finite_only:
    case ttl_permission::finite_or_indefinite:
        return true;
    }
    return false;
}

bool is_valid_foreground_policy(const foreground_policy value) noexcept {
    switch (value) {
    case foreground_policy::background:
    case foreground_policy::foreground_bound:
        return true;
    }
    return false;
}

result<void> validate_internal_context_options(const internal_context_options& options) noexcept {
    if (!options.owner.valid() || !is_valid_internal_priority(options.priority) ||
        !is_valid_ttl_permission(options.ttl) || !is_valid_foreground_policy(options.foreground)) {
        return invalid_argument();
    }
    return {};
}

result<void> validate_values(const channel_values& values) noexcept {
    if (!valid_trigger(values.left_trigger) || !valid_trigger(values.right_trigger) ||
        !valid_fraction(values.rumble.low_frequency) ||
        !valid_fraction(values.rumble.high_frequency) ||
        values.leds.player_leds > maximum_player_leds) {
        return invalid_argument();
    }
    return {};
}

result<void> validate_submit_request(const submit_request& request) noexcept {
    if (!valid_channel_mask(request.selected_channels) || request.sequence == 0U ||
        !valid_ttl(request.ttl, ttl_permission::finite_only)) {
        return invalid_argument();
    }
    return validate_values(request.values);
}

result<void> validate_internal_submit_request(const submit_request& request,
    const ttl_permission permission) noexcept {
    if (!valid_channel_mask(request.selected_channels) || request.sequence == 0U ||
        !valid_ttl(request.ttl, permission)) {
        return invalid_argument();
    }
    return validate_values(request.values);
}

result<void> validate_clear_request(const clear_request& request) noexcept {
    if (!valid_channel_mask(request.selected_channels) || request.sequence == 0U) {
        return invalid_argument();
    }
    return {};
}

result<optional_deadline> make_deadline(const deadline entry_time,
    const std::chrono::milliseconds ttl,
    const ttl_permission permission) noexcept {
    if (!valid_ttl(ttl, permission)) {
        return std::unexpected{error::invalid_argument};
    }
    if (ttl.count() == 0) {
        return optional_deadline{};
    }

    using duration = clock::duration;
    using rep = duration::rep;
    static_assert(std::numeric_limits<rep>::is_integer);
    static_assert(std::numeric_limits<rep>::is_signed);
    using conversion = std::ratio_divide<std::milli, duration::period>;
    const auto ttl_ticks = static_cast<long double>(ttl.count()) *
                           static_cast<long double>(conversion::num) /
                           static_cast<long double>(conversion::den);
    if (!std::isfinite(ttl_ticks) || ttl_ticks <= 0.0L ||
        ttl_ticks > static_cast<long double>(std::numeric_limits<rep>::max())) {
        return std::unexpected{error::deadline_overflow};
    }

    const auto delta = std::chrono::duration_cast<duration>(ttl);
    if (delta.count() <= 0) {
        return std::unexpected{error::deadline_overflow};
    }
    const auto entry_count = entry_time.time_since_epoch().count();
    const auto delta_count = delta.count();
    const auto maximum_count = deadline::max().time_since_epoch().count();
    if (entry_count > maximum_count - delta_count) {
        return std::unexpected{error::deadline_overflow};
    }
    return optional_deadline{deadline{duration{entry_count + delta_count}}};
}

bool is_expired(const optional_deadline expiry, const deadline now) noexcept {
    return expiry.has_value() && now >= *expiry;
}

result<winner_indices> select_winners(const std::span<const candidate_view> candidates,
    const deadline now) noexcept {
    if (candidates.size() > maximum_candidate_count) {
        return std::unexpected{error::capacity_exceeded};
    }

    winner_indices winners{};
    for (std::size_t candidate_index = 0U; candidate_index < candidates.size(); ++candidate_index) {
        const auto& candidate = candidates[candidate_index];
        const auto channel_index = index_for(candidate.source_channel);
        if (channel_index == channel_count || !is_valid_internal_priority(candidate.priority) ||
            candidate.accepted_commit_order == 0U) {
            return std::unexpected{error::invalid_argument};
        }
        if (is_expired(candidate.expires_at, now)) {
            continue;
        }

        const auto current_index = winners[channel_index];
        if (!current_index.has_value()) {
            winners[channel_index] = candidate_index;
            continue;
        }
        const auto& current = candidates[*current_index];
        if (candidate.priority > current.priority ||
            (candidate.priority == current.priority &&
                candidate.accepted_commit_order > current.accepted_commit_order)) {
            winners[channel_index] = candidate_index;
        }
    }
    return winners;
}

} // namespace inputnext::output::policy_detail

namespace inputnext::output {

result<finite_submission_window> make_finite_submission_window(
    const std::chrono::steady_clock::time_point entered_at,
    const std::chrono::milliseconds ttl) noexcept {
    const auto deadline =
        policy_detail::make_deadline(entered_at, ttl, ttl_permission::finite_only);
    if (!deadline.has_value()) {
        return std::unexpected{deadline.error()};
    }
    if (!deadline->has_value()) {
        return std::unexpected{error::deadline_overflow};
    }
    return finite_submission_window{entered_at, ttl, **deadline};
}

} // namespace inputnext::output
