#include "output/pcm_controlled_memory_port.hpp"
#include "output/pcm_test_access.hpp"

#include <inputnext/output_body_pcm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::output::make_pcm_preparer;
using inputnext::test::output::pcm_controlled_memory_port;
using inputnext::test::output::pcm_release_guard;

TEST_CASE("output.body_pcm_storage_keeps_its_store_after_the_preparer_is_destroyed",
    "[component][output][body-pcm]") {
    std::weak_ptr<pcm_controlled_memory_port> memory_lifetime;
    prepared_body_pcm retained;
    {
        auto memory = std::make_shared<pcm_controlled_memory_port>();
        memory_lifetime = memory;
        const auto preparer = make_pcm_preparer(memory);
        memory.reset();
        const std::array<float, 1U> input{0.75F};
        const auto result = preparer.prepare(body_pcm_input{.samples = input,
            .frame_count = 1U,
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
        REQUIRE(result.has_value());
        retained = *result;
    }

    CHECK_FALSE(memory_lifetime.expired());
    CHECK(retained.view().samples[0] == 0.75F);
    retained = prepared_body_pcm{};
    CHECK(memory_lifetime.expired());
}

TEST_CASE("output.body_pcm_budget_remains_charged_until_sample_deallocation_finishes",
    "[component][output][body-pcm]") {
    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    const auto preparer = make_pcm_preparer(memory);
    const std::array<float, 1U> input{0.5F};
    auto prepared = preparer.prepare(body_pcm_input{.samples = input,
        .frame_count = 1U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    REQUIRE(prepared.has_value());

    memory->block_next_deallocation();
    auto releasing = std::async(std::launch::async,
        [handle = std::move(*prepared)] mutable { handle = prepared_body_pcm{}; });
    pcm_release_guard release_on_exit{memory};
    REQUIRE(memory->wait_for_deallocation());
    CHECK((preparer.usage() == body_pcm_usage{.bytes = sizeof(float), .buffers = 1U}));

    memory->release_all();
    REQUIRE(releasing.wait_for(2s) == std::future_status::ready);
    releasing.get();
    CHECK(preparer.usage() == body_pcm_usage{});
}

} // namespace
