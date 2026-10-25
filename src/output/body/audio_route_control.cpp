#include "audio_route_control.hpp"

#include "output/authority/state.hpp"
#include "output/policy/body.hpp"

#include <expected>
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {
namespace {

template <typename State>
[[nodiscard]] result<void> qualify_target(State& state, const connection_target target) {
    auto pin = state.capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            static_cast<void>(state.retire(target));
            return std::unexpected{error::not_connected};
        }
        return std::unexpected{error::resources};
    }
    pin->release();
    return {};
}

template <typename State>
void reclaim_observed_retirements(State& state) {
    std::array<connection_target, 16U> visited{};
    std::size_t visited_count = 0U;
    for (const auto& route : state.audio_routes) {
        if (!route.facts.has_value()) {
            continue;
        }
        const auto target = route.facts->instance.target;
        const auto end = visited.begin() + static_cast<std::ptrdiff_t>(visited_count);
        if (std::ranges::find(visited.begin(), end, target) != end) {
            continue;
        }
        visited[visited_count] = target;
        ++visited_count;
        auto pin = state.capture(target);
        if (!pin.has_value() && pin.error() == inputnext::error::not_connected) {
            static_cast<void>(state.retire(target));
        } else if (pin.has_value()) {
            pin->release();
        }
    }
}

template <typename State>
[[nodiscard]] std::size_t matching_hd_body_count(const State& state,
    const audio_instance expected) noexcept {
    std::size_t count = 0U;
    for (const auto& entry : state.records) {
        if (entry.body.has_value() && entry.body->intent.route == body_actuator_route::hd &&
            entry.body->intent.identity.audio == expected) {
            ++count;
        }
    }
    return count;
}

} // namespace

audio_route_control::audio_route_control(authority& owner) noexcept : owner_(owner) {
}

result<audio_instance> audio_route_control::attach(const connection_target target,
    const body_route_support support,
    const bool ready) {
    auto& state = *owner_.implementation_;
    if (!target.valid() || !body_policy::valid_route_support(support)) {
        return std::unexpected{error::invalid_argument};
    }
    if (target.runtime != state.runtime) {
        return std::unexpected{error::not_connected};
    }

    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time();
    static_cast<void>(state.expire_target(target, evaluation_time));
    auto live = qualify_target(state, target);
    if (!live.has_value()) {
        return std::unexpected{live.error()};
    }
    if (state.find_audio_target(target) != nullptr) {
        return std::unexpected{error::busy};
    }

    auto free = std::ranges::find_if(state.audio_routes,
        [](const authority::implementation::audio_route_record& row) {
            return !row.facts.has_value();
        });
    if (free == state.audio_routes.end()) {
        reclaim_observed_retirements(state);
        live = qualify_target(state, target);
        if (!live.has_value()) {
            return std::unexpected{live.error()};
        }
        if (state.find_audio_target(target) != nullptr) {
            return std::unexpected{error::busy};
        }
        free = std::ranges::find_if(state.audio_routes,
            [](const authority::implementation::audio_route_record& row) {
                return !row.facts.has_value();
            });
    }
    if (free == state.audio_routes.end()) {
        return std::unexpected{error::capacity_exceeded};
    }

    const auto added_reservations = ready ? 2U : 1U;
    if (!state.ordinary_headroom(state.reservation_count() + added_reservations)) {
        return std::unexpected{error::logical_version_exhausted};
    }
    const auto id = state.take_audio_instance_id();
    if (!id.has_value()) {
        return std::unexpected{id.error()};
    }

    const audio_instance instance{
        .target = target,
        .authority = state.incarnation,
        .id = *id,
    };
    free->facts = body_policy::audio_route_facts{
        .instance = instance,
        .support = support,
        .ready = ready,
    };
    ++state.logical_version;
    return instance;
}

result<void> audio_route_control::set_ready(const audio_instance expected, const bool ready) {
    auto& state = *owner_.implementation_;
    if (!expected.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (expected.authority != state.incarnation || expected.target.runtime != state.runtime) {
        return std::unexpected{error::stale_audio};
    }

    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time();
    static_cast<void>(state.expire_target(expected.target, evaluation_time));
    auto* row = state.find_audio(expected);
    if (row == nullptr) {
        return std::unexpected{error::stale_audio};
    }
    if (ready) {
        auto live = qualify_target(state, expected.target);
        if (!live.has_value()) {
            return std::unexpected{live.error()};
        }
        row = state.find_audio(expected);
        if (row == nullptr) {
            return std::unexpected{error::stale_audio};
        }
    }
    if (!row->facts.has_value()) {
        return std::unexpected{error::stale_audio};
    }
    auto& facts = *row->facts;
    if (facts.ready == ready) {
        return {};
    }

    const auto invalidated_bodies = ready ? 0U : matching_hd_body_count(state, expected);
    const auto reservations_after =
        state.reservation_count() + (ready ? 1U : 0U) - (ready ? 0U : 1U + invalidated_bodies);
    if (!state.ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }

    const auto version = state.logical_version + 1U;
    facts.ready = ready;
    if (!ready) {
        static_cast<void>(state.latch_hd_bodies(expected.target,
            body_fallback_reason::not_ready,
            version,
            expected));
    }
    state.logical_version = version;
    return {};
}

result<void> audio_route_control::detach(const audio_instance expected) {
    auto& state = *owner_.implementation_;
    if (!expected.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (expected.authority != state.incarnation || expected.target.runtime != state.runtime) {
        return std::unexpected{error::stale_audio};
    }

    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time();
    static_cast<void>(state.expire_target(expected.target, evaluation_time));
    auto* row = state.find_audio(expected);
    if (row == nullptr || !row->facts.has_value()) {
        return std::unexpected{error::stale_audio};
    }
    const bool was_ready = row->facts->ready;
    const auto invalidated_bodies = matching_hd_body_count(state, expected);
    const auto reservations_after =
        state.reservation_count() - 1U - static_cast<std::size_t>(was_ready) - invalidated_bodies;
    if (!state.ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }

    const auto version = state.logical_version + 1U;
    static_cast<void>(state.latch_hd_bodies(expected.target,
        body_fallback_reason::instance_lost,
        version,
        expected));
    row->facts.reset();
    state.logical_version = version;
    return {};
}

} // namespace inputnext::output::detail
