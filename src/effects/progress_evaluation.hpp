#pragma once

#include "effects.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body_playback.hpp>

#include <cstdint>

namespace inputnext::gameinput_detail {
struct managed_backend_observation;
}

namespace inputnext::effects::detail {

enum class marker_kind : std::uint8_t { discrete, body, close };

struct progress_marker final {
    bool present{};
    marker_kind kind{marker_kind::discrete};
    context_handle external_context{};
    output::public_context native_context{};
    connection_target target{};
    publication_revision publication{};
    std::uint64_t logical_version{};
    output::mutation_receipt discrete{};
    output::body_playback_receipt body{};
    close_disposition disposition{close_disposition::normal};
    output::channel_mask selected_channels{};
};

[[nodiscard]] progress progress_response(connection_target target,
    publication_revision requested,
    progress_stage stage,
    status completion_status,
    publication_revision current = {},
    completion_class completion = completion_class::none,
    std::uint64_t attempt = 0U) noexcept;

[[nodiscard]] result<progress> evaluate_progress_observation(const progress_marker& marker,
    const gameinput_detail::managed_backend_observation& observation);

} // namespace inputnext::effects::detail
