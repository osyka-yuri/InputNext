#include "audio/native_types.hpp"
#include "audio/windows/wave_format.hpp"

#include <Windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace inputnext::audio_detail::windows {
namespace {

[[nodiscard]] native_audio_format quad_float() noexcept {
    return {.encoding = native_sample_encoding::ieee_float,
        .channels = 4U,
        .sample_rate = 48000U,
        .bits_per_sample = 32U,
        .valid_bits_per_sample = 32U,
        .block_align = 16U,
        .average_bytes_per_second = 768000U,
        .channel_mask = 0x33U,
        .extension_size = 22U,
        .extensible = true};
}

TEST_CASE("audio.windows_format.preserves_extensible_format_fields", "[unit][audio][windows]") {
    auto description = quad_float();
    constexpr std::array encodings{native_sample_encoding::ieee_float,
        native_sample_encoding::pcm_integer};
    for (const auto encoding : encodings) {
        description.encoding = encoding;
        const auto format = make_wave_format(description, native_operation::is_format_supported);
        REQUIRE(format.has_value());
        CHECK(format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
        CHECK(format->Format.cbSize == 22U);
        CHECK(format->Format.nChannels == 4U);
        CHECK(format->Format.nSamplesPerSec == 48000U);
        CHECK(format->dwChannelMask == 0x33U);
        const auto& subtype = encoding == native_sample_encoding::ieee_float
                                  ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                  : KSDATAFORMAT_SUBTYPE_PCM;
        CHECK(IsEqualGUID(format->SubFormat, subtype) != FALSE);
        CHECK(describe_wave_format(format->Format) == description);
    }
}

TEST_CASE("audio.windows_format.rejects_malformed_extensible_headers", "[unit][audio][windows]") {
    constexpr std::array<std::uint16_t, 4U> lengths{0U, 21U, 23U, 65535U};
    const native_error expected{.operation = native_operation::initialize,
        .native_code = static_cast<std::uint32_t>(E_INVALIDARG)};
    for (const auto length : lengths) {
        auto description = quad_float();
        description.extension_size = length;
        const auto format = make_wave_format(description, native_operation::initialize);
        REQUIRE(!format.has_value());
        CHECK(format.error() == expected);
    }
    auto description = quad_float();
    description.encoding = native_sample_encoding::unknown;
    CHECK(!make_wave_format(description, native_operation::initialize).has_value());
    description = quad_float();
    description.valid_bits_per_sample = 64U;
    CHECK(!make_wave_format(description, native_operation::initialize).has_value());
}

TEST_CASE("audio.windows_format.describes_non_extensible_formats_and_truncated_extensions",
    "[unit][audio][windows]") {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2U;
    format.nSamplesPerSec = 48000U;
    format.wBitsPerSample = 16U;
    CHECK(describe_wave_format(format).encoding == native_sample_encoding::pcm_integer);
    CHECK(!describe_wave_format(format).extensible);

    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    CHECK(describe_wave_format(format).encoding == native_sample_encoding::ieee_float);
    format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.cbSize = 21U;
    const auto truncated = describe_wave_format(format);
    CHECK(truncated.extensible);
    CHECK(truncated.encoding == native_sample_encoding::unknown);
    CHECK(truncated.channel_mask == 0U);
}

} // namespace
} // namespace inputnext::audio_detail::windows
