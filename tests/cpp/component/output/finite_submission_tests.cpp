#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/pcm_controlled_memory_port.hpp"
#include "output/pcm_test_access.hpp"
#include "values/required_value.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::output::detail::authority_test_access;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::make_pcm_preparer;
using inputnext::test::output::pcm_controlled_memory_port;
using inputnext::test::output::pcm_release_guard;

struct scripted_clock final {
    std::chrono::steady_clock::time_point expiry;
    mutable std::size_t reads{};
    mutable bool expire_on_third_read{};
};

[[nodiscard]] std::chrono::steady_clock::time_point scripted_now(
    const void* const context) noexcept {
    const auto& clock = *static_cast<const scripted_clock*>(context);
    ++clock.reads;
    if (clock.expire_on_third_read && clock.reads >= 3U) {
        return clock.expiry;
    }
    return std::chrono::steady_clock::time_point{};
}

[[nodiscard]] std::int64_t ticks_at(const std::chrono::milliseconds value) noexcept {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(value).count();
}

TEST_CASE("output.finite_submission_window_preserves_expiry_across_delayed_admission",
    "[component][output][submission][concurrency]") {
    const bool body_command = GENERATE(false, true);
    std::atomic<std::int64_t> ticks{};
    authority_fixture fixture{};
    authority_test_access::use_clock(fixture.owner, ticks);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto window = make_finite_submission_window({}, 5ms);
    REQUIRE(window.has_value());

    auto gate = authority_test_access::lock_gate(fixture.owner);
    std::promise<void> started;
    const auto entered = started.get_future();
    auto pending = std::async(std::launch::async, [&] -> result<void> {
        started.set_value();
        if (body_command) {
            const body_request request{.sequence = 1U,
                .ttl = 5ms,
                .preference = body_route_preference::classic,
                .fallback = {.low_frequency = 0.2F, .high_frequency = 0.3F}};
            const auto submitted = fixture.owner.submit_body(*opened, request, {}, *window);
            if (!submitted.has_value()) {
                return std::unexpected{submitted.error()};
            }
        } else {
            const auto request = inputnext::test::output::rumble_request(
                {.channels = rumble_mask, .sequence = 1U, .ttl = 5ms, .low = 0.2F, .high = 0.3F});
            const auto submitted = fixture.owner.submit(*opened, request, *window);
            if (!submitted.has_value()) {
                return std::unexpected{submitted.error()};
            }
        }
        return {};
    });
    const auto entered_status = entered.wait_for(2s);
    ticks.store(ticks_at(5ms), std::memory_order_release);
    gate.unlock();
    REQUIRE(entered_status == std::future_status::ready);
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    const auto rejected = pending.get();
    CHECK((!rejected.has_value() && rejected.error() == error::expired));
    const auto record = authority_test_access::observe_record(fixture.owner, opened->identity);
    const auto stored = inputnext::test::required_value(record);
    CHECK(stored.accepted_sequence == 0U);
    CHECK(stored.revisions == channel_revisions{});
    CHECK(!stored.body.has_value());
    CHECK(!stored.playback.has_value());
}

TEST_CASE("output.finite_submission_window_rechecks_exact_expiry_at_final_admission",
    "[component][output][submission]") {
    scripted_clock clock{};
    authority_fixture fixture{};
    authority_test_access::use_clock_callback(fixture.owner, &scripted_now, &clock);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    clock.reads = 0U;

    const auto original_window = make_finite_submission_window({}, 20ms);
    REQUIRE(original_window.has_value());
    const auto first_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 20ms, .low = 0.2F, .high = 0.4F});
    REQUIRE(fixture.owner.submit(*opened, first_request, *original_window).has_value());

    const auto window = make_finite_submission_window({}, 5ms);
    REQUIRE(window.has_value());
    const auto request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 2U, .ttl = 5ms, .low = 0.8F, .high = 0.9F});
    const auto mismatch = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 6ms, .low = 0.8F, .high = 0.9F}),
        *window);
    CHECK((!mismatch.has_value() && mismatch.error() == error::invalid_argument));

    clock.reads = 0U;
    clock.expiry = window->expires_at();
    clock.expire_on_third_read = true;
    const auto rejected = fixture.owner.submit(*opened, request, *window);
    CHECK((!rejected.has_value() && rejected.error() == error::expired));
    CHECK(clock.reads == 3U);

    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 1U);
    CHECK(info->revisions[2U] == 1U);
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    const auto rumble = inputnext::test::required_value(snapshot->rumble);
    CHECK((rumble.value == rumble_value{.low_frequency = 0.2F, .high_frequency = 0.4F}));
    CHECK(!make_finite_submission_window({}, 0ms).has_value());
}

TEST_CASE("output.finite_body_window_rechecks_expiry_after_route_planning",
    "[component][output][submission][body]") {
    scripted_clock clock{};
    authority_fixture fixture{};
    authority_test_access::use_clock_callback(fixture.owner, &scripted_now, &clock);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());

    const auto window = make_finite_submission_window({}, 5ms);
    REQUIRE(window.has_value());
    clock.expiry = window->expires_at();
    clock.reads = 0U;
    clock.expire_on_third_read = true;
    const body_request request{.sequence = 1U,
        .ttl = 5ms,
        .preference = body_route_preference::classic,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.3F}};
    const body_playback_update playback{.command = body_pcm_command::keep, .replacement = {}};

    const auto rejected = fixture.owner.submit_body(*opened, request, playback, *window);
    CHECK((!rejected.has_value() && rejected.error() == error::expired));
    CHECK(clock.reads == 3U);

    const auto record = authority_test_access::observe_record(fixture.owner, opened->identity);
    REQUIRE(record.has_value());
    const auto record_value = inputnext::test::required_value(record);
    CHECK(record_value.accepted_sequence == 0U);
    CHECK(!record_value.body.has_value());
    CHECK(!record_value.playback.has_value());
    CHECK(authority_test_access::next_body_lease_id(fixture.owner) == 1U);
    CHECK(fixture.owner.pcm_preparer().usage().buffers == 0U);
}

TEST_CASE("output.finite_body_window_expiring_during_pcm_preparation_reclaims_uncommitted_storage",
    "[component][output][submission][body][pcm]") {
    std::atomic<std::int64_t> ticks{};
    authority_fixture fixture{};
    authority_test_access::use_clock(fixture.owner, ticks);
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());

    const auto memory = std::make_shared<pcm_controlled_memory_port>();
    auto preparer = make_pcm_preparer(memory);
    authority_test_access::use_pcm_preparer(fixture.owner, preparer);

    constexpr std::array samples{0.25F, -0.5F};
    const body_pcm_input input{.samples = samples,
        .frame_count = samples.size(),
        .channels = 1U,
        .sample_rate = body_pcm_sample_rate,
        .gain = 1.0F};
    const auto window = make_finite_submission_window({}, 5ms);
    REQUIRE(window.has_value());
    const body_request request{.sequence = 1U,
        .ttl = 5ms,
        .preference = body_route_preference::classic,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.3F}};

    memory->block_next_allocation();
    auto preparing =
        std::async(std::launch::async, [&preparer, &input] { return preparer.prepare(input); });
    const pcm_release_guard release_on_exit{memory};
    REQUIRE(memory->wait_for_allocation());
    ticks.store(ticks_at(5ms), std::memory_order_release);
    memory->release_all();
    REQUIRE(preparing.wait_for(2s) == std::future_status::ready);
    auto prepared = preparing.get();
    REQUIRE(prepared.has_value());

    body_playback_update playback{.command = body_pcm_command::replace,
        .replacement = std::move(*prepared)};
    const auto rejected = fixture.owner.submit_body(*opened, request, playback, *window);
    CHECK((!rejected.has_value() && rejected.error() == error::expired));

    playback.replacement = prepared_body_pcm{};
    CHECK(fixture.owner.pcm_preparer().usage() == body_pcm_usage{});
    const auto record = authority_test_access::observe_record(fixture.owner, opened->identity);
    REQUIRE(record.has_value());
    const auto record_value = inputnext::test::required_value(record);
    CHECK(record_value.accepted_sequence == 0U);
    CHECK(!record_value.body.has_value());
    CHECK(!record_value.playback.has_value());
    CHECK(authority_test_access::next_body_lease_id(fixture.owner) == 1U);
}

} // namespace
