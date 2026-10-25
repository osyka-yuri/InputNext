#include "output/authority/state.hpp"
#include "output/policy/body.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output_authority.hpp>

#include <cstdint>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>

namespace inputnext::output {

std::size_t authority::implementation::hd_body_count(
    const connection_target target) const noexcept {
    std::size_t count = 0U;
    for (const auto& entry : records) {
        if (entry.state == phase::open && entry.identity.target == target &&
            entry.body.has_value() && entry.body->intent.route == body_actuator_route::hd) {
            ++count;
        }
    }
    return count;
}

bool authority::implementation::diagnostic_rumble_wins(const connection_target target,
    const policy_detail::deadline evaluation_time,
    const std::optional<focus::foreground_epoch> epoch) const noexcept {
    return std::ranges::any_of(records, [target, evaluation_time, epoch](const record& entry) {
        if (entry.state != phase::open || entry.identity.target != target ||
            entry.priority != internal_priority::diagnostic ||
            (entry.foreground == foreground_policy::foreground_bound &&
                (!epoch.has_value() || entry.opening_epoch != epoch))) {
            return false;
        }
        const auto& rumble = entry.channels[static_cast<std::size_t>(channel::rumble)];
        return rumble.active && !policy_detail::is_expired(rumble.expires_at, evaluation_time);
    });
}

std::size_t authority::implementation::latch_hd_bodies(const connection_target target,
    const body_fallback_reason reason,
    const std::uint64_t version,
    const std::optional<audio_instance> audio) noexcept {
    std::size_t changed = 0U;
    for (auto& entry : records) {
        if (entry.state != phase::open || entry.identity.target != target ||
            !entry.body.has_value() || entry.body->intent.route != body_actuator_route::hd ||
            (audio.has_value() && entry.body->intent.identity.audio != *audio)) {
            continue;
        }
        entry.body->intent =
            detail::body_policy::fallback_route(entry.body->intent, reason, version);
        ++changed;
    }
    return changed;
}

} // namespace inputnext::output
