#include "audio/native_types.hpp"
#include "audio/service/detail.hpp"

#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace {

using namespace inputnext::audio_detail;
using inputnext::native_guid;

[[nodiscard]] native_guid guid(const std::uint8_t first, const std::uint8_t last) noexcept {
    native_guid result{};
    result.bytes.front() = static_cast<std::byte>(first);
    result.bytes.back() = static_cast<std::byte>(last);
    return result;
}

[[nodiscard]] native_resource_key resource(const std::u16string_view endpoint,
    const native_guid container) {
    const auto id = fixed_haptic_endpoint_id::create(endpoint);
    if (!id.has_value()) {
        throw std::runtime_error{"controlled audio resource ID is invalid"};
    }
    return {.endpoint = *id, .container = container};
}

} // namespace

TEST_CASE("audio.resource_reservation_conflicts_by_exact_endpoint_or_container", "[unit][audio]") {
    const auto shared_container = guid(0x11U, 0x22U);
    const auto first = resource(u"endpoint-A", shared_container);
    const auto same_endpoint = resource(u"endpoint-A", guid(0x33U, 0x44U));
    const auto same_container = resource(u"endpoint-B", shared_container);
    const auto independent = resource(u"endpoint-B", guid(0x33U, 0x44U));

    CHECK(native_resource_conflicts(first, same_endpoint));
    CHECK(native_resource_conflicts(first, same_container));
    CHECK_FALSE(native_resource_conflicts(first, independent));
    CHECK_FALSE(native_resource_conflicts(first, resource(u"endpoint-a", guid(0x33U, 0x44U))));
}

TEST_CASE("audio.format_policy_exposes_only_the_canonical_extensible_quad", "[unit][audio]") {
    const auto expected = canonical_audio_format();
    CHECK(expected.extensible);
    CHECK(expected.encoding == native_sample_encoding::ieee_float);
    CHECK(expected.channels == 4U);
    CHECK(expected.sample_rate == 48000U);
    CHECK(expected.bits_per_sample == 32U);
    CHECK(expected.valid_bits_per_sample == 32U);
    CHECK(expected.block_align == 16U);
    CHECK(expected.average_bytes_per_second == 768000U);
    CHECK(expected.channel_mask == 0x33U);
    CHECK(expected.extension_size == 22U);

    auto malformed = expected;
    malformed.extension_size = 0U;
    CHECK(malformed != expected);
    malformed = expected;
    malformed.channel_mask = 0x0FU;
    CHECK(malformed != expected);
}

TEST_CASE("audio.telemetry_counters_saturate_instead_of_wrapping", "[unit][audio]") {
    std::atomic<bool> saturated{};
    saturating_counter counter{.saturation = &saturated};
    counter.value.store(UINT64_MAX - 2U, std::memory_order_relaxed);

    counter.add(5U);

    CHECK(counter.value.load(std::memory_order_relaxed) == UINT64_MAX);
    CHECK(saturated.load(std::memory_order_relaxed));
}
