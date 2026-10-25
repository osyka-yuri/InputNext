#include <inputnext/output.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using namespace inputnext::output;

TEST_CASE("output.body_pcm_preparation_owns_an_immutable_copy", "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    prepared_body_pcm prepared;
    {
        std::vector<float> caller_samples{0.25F, -0.5F, 0.75F, -1.0F};
        const body_pcm_input input{.samples = caller_samples,
            .frame_count = 2U,
            .channels = 2U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 0.5F};
        const auto result = preparer.prepare(input);
        REQUIRE(result.has_value());
        prepared = *result;

        caller_samples[0] = -0.75F;
        caller_samples.clear();
        caller_samples.shrink_to_fit();
    }

    REQUIRE(prepared.valid());
    const body_pcm_view view = prepared.view();
    CHECK(view.frame_count == 2U);
    CHECK(view.channels == 2U);
    CHECK(view.sample_rate == body_pcm_sample_rate);
    CHECK(view.gain == 0.5F);
    REQUIRE(view.samples.size() == 4U);
    CHECK(view.samples[0] == 0.25F);
    CHECK(view.samples[1] == -0.5F);
    CHECK(view.samples[2] == 0.75F);
    CHECK(view.samples[3] == -1.0F);
    CHECK(prepared.storage_bytes() == 4ULL * sizeof(float));

    const prepared_body_pcm copy = prepared;
    CHECK(copy.view().samples.data() == prepared.view().samples.data());
    CHECK(preparer.owns(copy));
    const body_pcm_preparer other_store;
    CHECK_FALSE(other_store.owns(copy));
}

TEST_CASE("output.body_pcm_preparation_rejects_malformed_descriptors",
    "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    const std::array<float, 2U> samples{0.0F, 0.5F};
    const auto prepare = [&preparer](const body_pcm_input& input) {
        return preparer.prepare(input);
    };
    const auto expect_invalid = [&prepare](const body_pcm_input& input) {
        const auto rejected = prepare(input);
        CHECK((!rejected.has_value() && rejected.error() == error::invalid_argument));
    };

    expect_invalid(body_pcm_input{.samples = samples,
        .frame_count = 0U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    expect_invalid(body_pcm_input{.samples = samples,
        .frame_count = 1U,
        .channels = 0U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    expect_invalid(body_pcm_input{.samples = samples,
        .frame_count = 1U,
        .channels = 3U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    expect_invalid(body_pcm_input{.samples = samples,
        .frame_count = 1U,
        .channels = 2U,
        .sample_rate = body_pcm_sample_rate + 1U,
        .gain = 1.0F});
    expect_invalid(body_pcm_input{.samples = std::span<const float>{samples}.first(1U),
        .frame_count = 2U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});

    const auto frame_product_overflow = prepare(body_pcm_input{.samples = {},
        .frame_count = std::numeric_limits<std::uint64_t>::max(),
        .channels = 2U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    CHECK((!frame_product_overflow.has_value() &&
           frame_product_overflow.error() == error::invalid_argument));

    const auto byte_product_overflow = prepare(body_pcm_input{.samples = {},
        .frame_count = std::numeric_limits<std::uint64_t>::max() / 2U,
        .channels = 2U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    CHECK((!byte_product_overflow.has_value() &&
           byte_product_overflow.error() == error::invalid_argument));
}

TEST_CASE("output.body_pcm_preparation_rejects_invalid_samples_and_gain",
    "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    const std::array<float, 1U> sample{0.0F};
    const auto prepare = [&preparer, &sample](const float gain) {
        return preparer.prepare(body_pcm_input{.samples = sample,
            .frame_count = 1U,
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = gain});
    };

    for (const float gain : {std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -0.01F,
             1.01F}) {
        const auto rejected = prepare(gain);
        CHECK((!rejected.has_value() && rejected.error() == error::invalid_argument));
    }
    CHECK(prepare(0.0F).has_value());
    CHECK(prepare(1.0F).has_value());

    for (const float invalid_sample : {std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
             -std::numeric_limits<float>::infinity(),
             -1.01F,
             1.01F}) {
        const std::array<float, 1U> invalid{invalid_sample};
        const auto rejected = preparer.prepare(body_pcm_input{.samples = invalid,
            .frame_count = 1U,
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
        CHECK((!rejected.has_value() && rejected.error() == error::invalid_argument));
    }
}

TEST_CASE("output.body_pcm_preparation_accepts_mono_and_stereo_over_thirty_seconds",
    "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    constexpr std::uint64_t frames = 48000ULL * 31ULL;
    std::vector<float> mono(static_cast<std::size_t>(frames), 0.1F);
    std::vector<float> stereo(static_cast<std::size_t>(frames * 2U), -0.1F);

    const auto mono_clip = preparer.prepare(body_pcm_input{.samples = mono,
        .frame_count = frames,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    REQUIRE(mono_clip.has_value());
    const auto stereo_clip = preparer.prepare(body_pcm_input{.samples = stereo,
        .frame_count = frames,
        .channels = 2U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 0.75F});
    REQUIRE(stereo_clip.has_value());
    CHECK(mono_clip->view().frame_count == frames);
    CHECK(mono_clip->view().channels == 1U);
    CHECK(stereo_clip->view().frame_count == frames);
    CHECK(stereo_clip->view().channels == 2U);
}

} // namespace
