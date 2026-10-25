#pragma once

#include "output/playback/body_playback_bank.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace inputnext::test::output {

namespace output_api = ::inputnext::output;

struct playback_identity_spec final {
    std::uint64_t target_value{};
    std::uint64_t lease_value{};
    std::uint64_t clip_revision{};
    output_api::audio_instance audio{};
};

template <typename Value>
[[nodiscard]] Value take_optional_value(std::optional<Value>& value) {
    if (!value.has_value()) {
        throw std::runtime_error{"Test expected a present optional value"};
    }
    return std::move(value.value());
}

[[nodiscard]] inline runtime_id playback_runtime_id() noexcept {
    runtime_id value{};
    value.bytes[0] = std::byte{1U};
    return value;
}

[[nodiscard]] inline output_api::authority_incarnation playback_authority_id() noexcept {
    output_api::authority_incarnation value{};
    value.bytes[0] = std::byte{1U};
    return value;
}

[[nodiscard]] inline inputnext::connection_target playback_target(
    const std::uint64_t value) noexcept {
    return inputnext::connection_target{.runtime = playback_runtime_id(),
        .device = device_key{value},
        .connection = connection_id{value}};
}

[[nodiscard]] inline output_api::audio_instance playback_audio(
    const inputnext::connection_target target,
    const output_api::audio_instance_id id) noexcept {
    return output_api::audio_instance{.target = target,
        .authority = playback_authority_id(),
        .id = id};
}

[[nodiscard]] inline output_api::body_playback_identity playback_identity(
    const playback_identity_spec spec) noexcept {
    const auto target = playback_target(spec.target_value);
    const auto authority = playback_authority_id();
    const output_api::context_identity context{.target = target,
        .incarnation = authority,
        .id = output_api::context_id{spec.lease_value},
        .kind = output_api::owner_class::public_game,
        .internal_owner = {}};
    return output_api::body_playback_identity{
        .lease = output_api::body_lease_identity{.context = context,
            .lease = output_api::body_lease_id{spec.lease_value},
            .audio = spec.audio},
        .clip_revision = spec.clip_revision};
}

[[nodiscard]] inline output_api::backend_handle playback_backend(
    const inputnext::connection_target target,
    const output_api::backend_id backend) noexcept {
    const output_api::execution_owner owner{.authority = playback_authority_id(),
        .id = output_api::execution_id{1U}};
    return output_api::backend_handle{.target = target,
        .incarnation = output_api::backend_incarnation{.owner = owner, .id = backend}};
}

[[nodiscard]] inline output_api::result<output_api::prepared_body_pcm> prepare_mono_pcm(
    output_api::body_pcm_preparer& preparer,
    const std::span<const float> samples,
    const float gain = 1.0F) {
    return preparer.prepare(output_api::body_pcm_input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = output_api::body_pcm_sample_rate,
        .gain = gain});
}

[[nodiscard]] inline bool install_voice(output_api::detail::body_playback_bank& bank,
    output_api::detail::body_playback_retirement_batch& displaced,
    output_api::body_playback_identity identity,
    output_api::prepared_body_pcm&& samples,
    std::optional<output_api::body_playback_identity> previous = std::nullopt) noexcept {
    const auto current = previous.has_value() ? bank.find_current(previous->lease) : std::nullopt;
    const auto selection = bank.select_install_slot(identity.lease.context.target, current);
    if (!selection.has_value()) {
        return false;
    }
    return bank.install(*selection, previous, identity, std::move(samples), displaced);
}

} // namespace inputnext::test::output
