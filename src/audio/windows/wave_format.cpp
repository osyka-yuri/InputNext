#include "wave_format.hpp"

#include "audio/native_types.hpp"

#include <Windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace inputnext::audio_detail::windows {

native_audio_format describe_wave_format(const WAVEFORMATEX& value) noexcept {
    native_audio_format result{.encoding = native_sample_encoding::unknown,
        .channels = value.nChannels,
        .sample_rate = value.nSamplesPerSec,
        .bits_per_sample = value.wBitsPerSample,
        .valid_bits_per_sample = value.wBitsPerSample,
        .block_align = value.nBlockAlign,
        .average_bytes_per_second = value.nAvgBytesPerSec,
        .channel_mask = 0U,
        .extension_size = value.cbSize,
        .extensible = false};

    if (value.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        result.extensible = true;
        if (static_cast<std::size_t>(value.cbSize) <
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return result;
        }
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(value);
        result.valid_bits_per_sample = extensible.Samples.wValidBitsPerSample;
        result.channel_mask = extensible.dwChannelMask;
        if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE) {
            result.encoding = native_sample_encoding::ieee_float;
        } else if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE) {
            result.encoding = native_sample_encoding::pcm_integer;
        }
    } else if (value.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        result.encoding = native_sample_encoding::ieee_float;
    } else if (value.wFormatTag == WAVE_FORMAT_PCM) {
        result.encoding = native_sample_encoding::pcm_integer;
    }
    return result;
}

native_result<WAVEFORMATEXTENSIBLE> make_wave_format(const native_audio_format& value,
    const native_operation operation) noexcept {
    if (!value.extensible ||
        static_cast<std::size_t>(value.extension_size) !=
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
        value.channels == 0U || value.sample_rate == 0U || value.bits_per_sample == 0U ||
        value.valid_bits_per_sample == 0U || value.valid_bits_per_sample > value.bits_per_sample ||
        value.block_align == 0U || value.average_bytes_per_second == 0U) {
        return std::unexpected{native_error{.operation = operation,
            .native_code = static_cast<std::uint32_t>(E_INVALIDARG)}};
    }

    WAVEFORMATEXTENSIBLE result{};
    result.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result.Format.nChannels = value.channels;
    result.Format.nSamplesPerSec = value.sample_rate;
    result.Format.nAvgBytesPerSec = value.average_bytes_per_second;
    result.Format.nBlockAlign = value.block_align;
    result.Format.wBitsPerSample = value.bits_per_sample;
    result.Format.cbSize = value.extension_size;
    result.Samples.wValidBitsPerSample = value.valid_bits_per_sample;
    result.dwChannelMask = value.channel_mask;
    if (value.encoding == native_sample_encoding::ieee_float) {
        result.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else if (value.encoding == native_sample_encoding::pcm_integer) {
        result.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    } else {
        return std::unexpected{native_error{.operation = operation,
            .native_code = static_cast<std::uint32_t>(E_INVALIDARG)}};
    }
    return result;
}

} // namespace inputnext::audio_detail::windows
