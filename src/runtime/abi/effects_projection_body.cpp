#include "effects_projection.hpp"

#include "effects/effects.hpp"
#include "effects_projection_support.hpp"
#include "output/pcm/body_pcm_internal.hpp"

#include <input_next_effects.h>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace inputnext::runtime_detail::effects_projection {
namespace {

using effects_projection_support::valid_fraction;
using effects_projection_support::valid_ttl;

[[nodiscard]] bool valid_pcm_empty(const input_next_effects_pcm_v1& value) noexcept {
    const bool empty = value.struct_size == sizeof(value) && value.frame_count == 0U &&
                       value.channels == 0U && value.sample_rate == 0U && value.gain == 0.0F &&
                       value.reserved == 0U && value.samples == nullptr;
#if UINTPTR_MAX == UINT32_MAX
    return empty && value.pointer_padding == 0U;
#else
    return empty;
#endif
}

[[nodiscard]] bool parse_pcm_replacement(const input_next_effects_pcm_v1& value,
    output::body_pcm_input& out) noexcept {
    if (value.struct_size != sizeof(value) || value.reserved != 0U || value.frame_count == 0U ||
        (value.channels != 1U && value.channels != 2U) ||
        value.sample_rate != output::body_pcm_sample_rate || !std::isfinite(value.gain) ||
        value.gain < 0.0F || value.gain > 1.0F || value.samples == nullptr) {
        return false;
    }
#if UINTPTR_MAX == UINT32_MAX
    if (value.pointer_padding != 0U) {
        return false;
    }
#endif

    const auto frame_count = static_cast<std::uint64_t>(value.frame_count);
    const auto channels = static_cast<std::uint64_t>(value.channels);
    const auto sample_count_64 = frame_count * channels;
    constexpr auto maximum_samples_by_budget =
        output::body_pcm_storage_budget_bytes / sizeof(float);
    static_assert(std::numeric_limits<std::size_t>::max() >= output::body_pcm_storage_budget_bytes);
    if (sample_count_64 > maximum_samples_by_budget) {
        return false;
    }

    const auto sample_count = static_cast<std::size_t>(sample_count_64);
    const auto samples = std::span<const float>{value.samples, sample_count};
    out = output::body_pcm_input{.samples = samples,
        .frame_count = value.frame_count,
        .channels = value.channels,
        .sample_rate = value.sample_rate,
        .gain = value.gain};
    const auto descriptor = output::detail::validate_pcm_descriptor(out);
    if (!descriptor.has_value()) {
        return false;
    }
    return output::detail::validate_pcm_samples(samples).has_value();
}

[[nodiscard]] bool write_fallback(const output::body_fallback_reason value,
    std::uint32_t& out) noexcept {
    switch (value) {
    case output::body_fallback_reason::none:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_NONE;
        return true;
    case output::body_fallback_reason::not_ready:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_NOT_READY;
        return true;
    case output::body_fallback_reason::unsupported_transport:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_UNSUPPORTED_TRANSPORT;
        return true;
    case output::body_fallback_reason::instance_lost:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_INSTANCE_LOST;
        return true;
    case output::body_fallback_reason::output_failed:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_OUTPUT_FAILED;
        return true;
    case output::body_fallback_reason::preempted:
        out = INPUT_NEXT_EFFECTS_BODY_FALLBACK_PREEMPTED;
        return true;
    }
    return false;
}

} // namespace

bool parse_body_request(const input_next_effects_body_request_v1& value,
    effects::body_request& out) noexcept {
    out = {};
    effects::context_handle context{};
    if (value.struct_size != sizeof(value) || !valid_ttl(value.ttl_ms) || value.sequence == 0U ||
        !parse_context(value.context, context) ||
        !valid_fraction(value.fallback_rumble.low_frequency) ||
        !valid_fraction(value.fallback_rumble.high_frequency)) {
        return false;
    }

    output::body_route_preference preference{};
    switch (value.route_preference) {
    case INPUT_NEXT_EFFECTS_BODY_ROUTE_CLASSIC:
        preference = output::body_route_preference::classic;
        break;
    case INPUT_NEXT_EFFECTS_BODY_ROUTE_PREFER_HD:
        preference = output::body_route_preference::prefer_hd;
        break;
    default:
        return false;
    }

    output::body_pcm_command command{};
    switch (value.pcm_command) {
    case INPUT_NEXT_EFFECTS_BODY_PCM_KEEP:
        command = output::body_pcm_command::keep;
        if (!valid_pcm_empty(value.pcm)) {
            return false;
        }
        break;
    case INPUT_NEXT_EFFECTS_BODY_PCM_REPLACE: {
        command = output::body_pcm_command::replace;
        output::body_pcm_input replacement{};
        if (!parse_pcm_replacement(value.pcm, replacement)) {
            return false;
        }
        out.replacement = replacement;
        break;
    }
    case INPUT_NEXT_EFFECTS_BODY_PCM_STOP:
        command = output::body_pcm_command::stop;
        if (!valid_pcm_empty(value.pcm)) {
            return false;
        }
        break;
    default:
        return false;
    }

    out.context = context;
    out.lease_id = value.lease_id;
    out.audio_instance_id = value.audio_instance_id;
    out.sequence = value.sequence;
    out.ttl = std::chrono::milliseconds{value.ttl_ms};
    out.preference = preference;
    out.fallback = {.low_frequency = value.fallback_rumble.low_frequency,
        .high_frequency = value.fallback_rumble.high_frequency};
    out.pcm_command = command;
    return true;
}

bool parse_body_clear_request(const input_next_effects_body_clear_v1& value,
    effects::body_clear_request& out) noexcept {
    out = {};
    effects::context_handle context{};
    if (value.struct_size != sizeof(value) || value.reserved != 0U || value.lease_id == 0U ||
        value.sequence == 0U || !parse_context(value.context, context)) {
        return false;
    }
    out = {.context = context,
        .lease_id = value.lease_id,
        .audio_instance_id = value.audio_instance_id,
        .sequence = value.sequence};
    return true;
}

bool write_body_receipt(const effects::body_receipt& value,
    input_next_effects_body_receipt_v1& out) noexcept {
    if (!value.context.runtime.valid() || value.context.id == 0U || !value.target.valid() ||
        value.lease_id == 0U || value.sequence == 0U || value.publication.value == 0U) {
        return false;
    }
    std::uint32_t route{};
    switch (value.route) {
    case output::body_actuator_route::classic:
        route = INPUT_NEXT_EFFECTS_BODY_EFFECTIVE_CLASSIC;
        break;
    case output::body_actuator_route::hd:
        route = INPUT_NEXT_EFFECTS_BODY_EFFECTIVE_HD;
        break;
    default:
        return false;
    }
    std::uint32_t fallback{};
    if (!write_fallback(value.fallback, fallback)) {
        return false;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.effective_route = route;
    write_context(value.context, out.context);
    if (!write_target(value.target, out.target)) {
        return false;
    }
    out.lease_id = value.lease_id;
    out.audio_instance_id = value.audio_instance_id;
    out.sequence = value.sequence;
    out.publication_revision = value.publication.value;
    out.fallback_reason = fallback;
    return true;
}

} // namespace inputnext::runtime_detail::effects_projection
