#pragma once

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <expected>

namespace inputnext::output::detail {

[[nodiscard]] inline result<void> validate_audio_identity(const context_identity context,
    const audio_instance audio) noexcept {
    if (audio.unbound()) {
        return {};
    }
    if (!audio.valid() || audio.target != context.target ||
        audio.authority != context.incarnation) {
        return std::unexpected{error::stale_audio};
    }
    return {};
}

template <typename State>
[[nodiscard]] auto find_public_body_context(State& state,
    const context_identity identity) noexcept {
    using record_pointer = decltype(state.records.data());
    if (!state.valid_context_identity(identity, owner_class::public_game)) {
        return result<record_pointer>{std::unexpected{error::stale_context}};
    }
    auto* const entry = state.find_open(identity);
    if (entry == nullptr) {
        return result<record_pointer>{std::unexpected{
            state.find_retained(identity) != nullptr ? error::revoked : error::stale_context}};
    }
    return result<record_pointer>{entry};
}

} // namespace inputnext::output::detail
