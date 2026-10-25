#include "body_pcm_internal.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace inputnext::output::detail {

result<pcm_descriptor> validate_pcm_descriptor(const body_pcm_input& input) noexcept {
    if (input.frame_count == 0U || (input.channels != 1U && input.channels != 2U) ||
        input.sample_rate != body_pcm_sample_rate || !std::isfinite(input.gain) ||
        input.gain < 0.0F || input.gain > 1.0F) {
        return std::unexpected{error::invalid_argument};
    }

    constexpr auto uint64_max = std::numeric_limits<std::uint64_t>::max();
    if (input.frame_count > uint64_max / input.channels) {
        return std::unexpected{error::invalid_argument};
    }

    const std::uint64_t sample_count_64 = input.frame_count * input.channels;
    if (sample_count_64 > uint64_max / sizeof(float)) {
        return std::unexpected{error::invalid_argument};
    }
    const std::uint64_t storage_bytes_64 = sample_count_64 * sizeof(float);
    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    if (storage_bytes_64 > size_max) {
        return std::unexpected{error::invalid_argument};
    }

    const auto sample_count = static_cast<std::size_t>(sample_count_64);
    if (input.samples.size() != sample_count) {
        return std::unexpected{error::invalid_argument};
    }

    return pcm_descriptor{.sample_count = sample_count, .storage_bytes = storage_bytes_64};
}

result<void> validate_pcm_samples(const std::span<const float> samples) noexcept {
    if (!std::ranges::all_of(samples, [](const float sample) {
            return std::isfinite(sample) && sample >= -1.0F && sample <= 1.0F;
        })) {
        return std::unexpected{error::invalid_argument};
    }
    return {};
}

} // namespace inputnext::output::detail
