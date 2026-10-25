#include "output/authority_fixture.hpp"

#include <inputnext/output_authority.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

namespace {

using namespace inputnext::output;

TEST_CASE("output.pcm_preparer_and_samples_remain_valid_after_authority_destruction",
    "[integration][output][body-pcm]") {
    std::optional<body_pcm_preparer> retained_preparer;
    prepared_body_pcm retained_samples;
    {
        const inputnext::test::output::authority_fixture fixture;
        retained_preparer.emplace(fixture.owner.pcm_preparer());
        constexpr std::array samples{0.25F, -0.75F};
        const auto prepared = retained_preparer->prepare(body_pcm_input{.samples = samples,
            .frame_count = samples.size(),
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
        REQUIRE(prepared.has_value());
        retained_samples = *prepared;
    }

    REQUIRE(retained_preparer.has_value());
    CHECK(retained_preparer->owns(retained_samples));
    CHECK(retained_preparer->usage().buffers == 1U);
    CHECK(retained_samples.view().samples[0U] == 0.25F);
    CHECK(retained_samples.view().samples[1U] == -0.75F);
    retained_samples = {};
    CHECK(retained_preparer->usage() == body_pcm_usage{});
}

} // namespace
