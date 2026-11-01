#include "output/pcm_controlled_memory_port.hpp"
#include "output/pcm_test_access.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <future>
#include <limits>
#include <memory>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::output::make_pcm_preparer;
using inputnext::test::output::pcm_controlled_memory_port;
using inputnext::test::output::pcm_release_guard;

TEST_CASE("output.body_pcm_store_enforces_exact_byte_budget_and_shared_copy_charges",
    "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    constexpr auto exact_samples =
        static_cast<std::size_t>(body_pcm_storage_budget_bytes / sizeof(float));
    std::vector<float> exact_input(exact_samples, 0.25F);
    auto exact = preparer.prepare(body_pcm_input{.samples = exact_input,
        .frame_count = exact_samples,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    REQUIRE(exact.has_value());
    CHECK(exact->storage_bytes() == body_pcm_storage_budget_bytes);
    CHECK((
        preparer.usage() == body_pcm_usage{.bytes = body_pcm_storage_budget_bytes, .buffers = 1U}));

    prepared_body_pcm shared_copy = *exact;
    CHECK(preparer.usage().buffers == 1U);
    std::vector<float> over_budget_input(exact_samples + 1U, 0.0F);
    const auto over_budget = preparer.prepare(body_pcm_input{.samples = over_budget_input,
        .frame_count = exact_samples + 1U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    CHECK((!over_budget.has_value() && over_budget.error() == error::capacity_exceeded));

    exact = std::unexpected{error::invalid_argument};
    CHECK(preparer.usage().bytes == body_pcm_storage_budget_bytes);
    shared_copy = prepared_body_pcm{};
    CHECK(preparer.usage() == body_pcm_usage{});
}

TEST_CASE("output.body_pcm_store_bounds_distinct_buffers_and_counts_shared_copies_once",
    "[component][output][body-pcm]") {
    const body_pcm_preparer preparer;
    constexpr std::size_t buffer_count = body_pcm_buffer_capacity;
    const std::array<float, 1U> input{0.1F};
    std::array<prepared_body_pcm, buffer_count> handles{};
    for (std::size_t index = 0U; index < buffer_count; ++index) {
        const auto prepared = preparer.prepare(body_pcm_input{.samples = input,
            .frame_count = 1U,
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
        REQUIRE(prepared.has_value());
        handles[index] = *prepared;
    }
    CHECK(preparer.usage().buffers == buffer_count);

    const prepared_body_pcm shared = handles[0U];
    handles[0U] = prepared_body_pcm{};
    CHECK(preparer.usage().buffers == buffer_count);
    const auto rejected = preparer.prepare(body_pcm_input{.samples = input,
        .frame_count = 1U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F});
    CHECK((!rejected.has_value() && rejected.error() == error::capacity_exceeded));

    handles[1U] = prepared_body_pcm{};
    CHECK(preparer.usage().buffers == buffer_count - 1U);
    CHECK(shared.valid());
    for (auto& handle : handles) {
        handle = prepared_body_pcm{};
    }
    CHECK(preparer.usage().buffers == 1U);
}

TEST_CASE("output.body_pcm_store_reserves_before_allocation_without_holding_the_budget_lock",
    "[component][output][body-pcm]") {
    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    const auto preparer = make_pcm_preparer(memory);
    const std::array<float, 1U> input{0.5F};
    memory->block_next_allocation();
    auto preparing = std::async(std::launch::async, [&preparer, &input] {
        return preparer.prepare(body_pcm_input{.samples = input,
            .frame_count = 1U,
            .channels = 1U,
            .sample_rate = body_pcm_sample_rate,
            .gain = 1.0F});
    });
    pcm_release_guard release_on_exit{memory};
    REQUIRE(memory->wait_for_allocation());

    auto reading_usage = std::async(std::launch::async, [&preparer] { return preparer.usage(); });
    const auto lock_was_free = reading_usage.wait_for(500ms) == std::future_status::ready;
    CHECK(lock_was_free);
    memory->release_all();
    CHECK((reading_usage.get() == body_pcm_usage{.bytes = sizeof(float), .buffers = 1U}));
    REQUIRE(preparing.wait_for(2s) == std::future_status::ready);
    const auto prepared = preparing.get();
    REQUIRE(prepared.has_value());
    CHECK(preparer.usage().buffers == 1U);
}

TEST_CASE("output.body_pcm_store_rolls_back_allocation_and_copied_sample_failures",
    "[component][output][body-pcm]") {
    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    const auto preparer = make_pcm_preparer(memory);
    const std::array<float, 1U> input{0.25F};
    const body_pcm_input descriptor{.samples = input,
        .frame_count = 1U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F};

    memory->fail_next_allocation();
    const auto allocation_failure = preparer.prepare(descriptor);
    CHECK((!allocation_failure.has_value() && allocation_failure.error() == error::resources));
    CHECK(preparer.usage() == body_pcm_usage{});

    const std::array<float, 1U> invalid_sample{std::numeric_limits<float>::quiet_NaN()};
    const body_pcm_input invalid_descriptor{.samples = invalid_sample,
        .frame_count = 1U,
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F};
    const auto invalid_copy = preparer.prepare(invalid_descriptor);
    CHECK((!invalid_copy.has_value() && invalid_copy.error() == error::invalid_argument));
    CHECK(memory->deallocation_calls() == 1U);
    CHECK(preparer.usage() == body_pcm_usage{});
}

} // namespace
