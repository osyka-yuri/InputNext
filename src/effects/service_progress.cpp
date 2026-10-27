#include "implementation.hpp"

#include "backend_observation.hpp"

#include "effects/effects.hpp"
#include "progress_evaluation.hpp"

#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>

#include <cstdint>
#include <expected>
#include <mutex>

namespace inputnext::effects {
namespace {

using detail::marker_kind;
using detail::progress_marker;
using detail::progress_response;

template <typename State>
[[nodiscard]] progress_marker find_marker(const State& state,
    const connection_target target,
    const publication_revision publication) noexcept {
    for (const auto& record : state.contexts) {
        if ((record.phase != context_phase::open &&
                record.phase != context_phase::closed_retained) ||
            record.target != target) {
            continue;
        }
        if (record.latest_discrete.present && record.latest_discrete.publication == publication) {
            return progress_marker{.present = true,
                .kind = marker_kind::discrete,
                .external_context = record.external_context,
                .native_context = record.native_context,
                .target = record.target,
                .publication = publication,
                .logical_version = record.latest_discrete.receipt.logical_version,
                .discrete = record.latest_discrete.receipt,
                .selected_channels = record.latest_discrete.selected_channels};
        }
        if (record.latest_body.present && record.latest_body.publication == publication) {
            return progress_marker{.present = true,
                .kind = marker_kind::body,
                .external_context = record.external_context,
                .native_context = record.native_context,
                .target = record.target,
                .publication = publication,
                .logical_version = record.latest_body.receipt.body.logical_version,
                .body = record.latest_body.receipt};
        }
        if (record.external_close.has_value() &&
            record.external_close->publication == publication) {
            std::uint64_t logical_version{};
            if (record.native_close.has_value()) {
                logical_version = record.native_close->logical_revocation_version;
            }
            return progress_marker{.present = true,
                .kind = marker_kind::close,
                .external_context = record.external_context,
                .native_context = record.native_context,
                .target = record.target,
                .publication = publication,
                .logical_version = logical_version,
                .disposition = record.external_close->disposition};
        }
    }
    return {};
}

[[nodiscard]] progress backend_unavailable(const connection_target target,
    const publication_revision publication,
    const status backend_status) noexcept {
    const bool pending = backend_status == status::busy;
    return progress_response(target,
        publication,
        pending ? progress_stage::dispatch_pending : progress_stage::not_deliverable,
        pending ? status::pending : backend_status);
}

template <typename State>
[[nodiscard]] result<progress> evaluate_progress(State& state, const progress_marker& marker) {
    if (marker.disposition == close_disposition::connection_retired ||
        state.target_retired(marker.target)) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::connection_retired,
            status::source_retired);
    }

    gameinput_detail::managed_backend_observation observation{};
    const auto observed =
        current_backend(state.dependencies.connections, marker.target, observation);
    if (observed != status::ok) {
        return backend_unavailable(marker.target, marker.publication, observed);
    }

    return detail::evaluate_progress_observation(marker, observation);
}

template <typename State>
[[nodiscard]] bool marker_still_retained(const State& state,
    const progress_marker& marker) noexcept {
    const auto current = find_marker(state, marker.target, marker.publication);
    return current.present && current.external_context == marker.external_context &&
           current.kind == marker.kind;
}

} // namespace

result<progress> service::query_progress(const connection_target target,
    const publication_revision publication) {
    auto& state = *implementation_;
    if (!target.valid() || target.runtime != state.identity || publication.value == 0U) {
        return std::unexpected{status::invalid_argument};
    }

    progress_marker marker{};
    {
        std::scoped_lock lock{state.gate};
        marker = find_marker(state, target, publication);
    }
    if (!marker.present) {
        return progress_response(target, publication, progress_stage::unknown, status::no_data);
    }

    const auto evaluated = evaluate_progress(state, marker);
    if (!evaluated.has_value()) {
        return std::unexpected{evaluated.error()};
    }
    std::scoped_lock lock{state.gate};
    if (!marker_still_retained(state, marker)) {
        return progress_response(target, publication, progress_stage::unknown, status::no_data);
    }
    return evaluated.value();
}

} // namespace inputnext::effects
