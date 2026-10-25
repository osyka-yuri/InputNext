#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/controlled_start.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_start;
using inputnext::test::output::release_and_join;
using output_error = inputnext::output::error;

struct paired_intent final {
    std::uint64_t sequence;
    float trigger_strength;
    float low_frequency;
    float high_frequency;
};

[[nodiscard]] submit_request paired_request(paired_intent intent) noexcept {
    auto request = inputnext::test::output::rumble_request({
        .channels = left_trigger_mask | rumble_mask,
        .sequence = intent.sequence,
        .ttl = 0ms,
        .low = intent.low_frequency,
        .high = intent.high_frequency,
    });
    request.values.left_trigger.mode = trigger_mode::feedback;
    request.values.left_trigger.strength = intent.trigger_strength;
    return request;
}

[[nodiscard]] submit_request led_request(std::uint64_t sequence,
    std::array<std::uint8_t, 3U> rgb) noexcept {
    submit_request request;
    request.selected_channels = leds_mask;
    request.sequence = sequence;
    request.ttl = 0ms;
    request.values.leds.rgb = rgb;
    return request;
}

[[nodiscard]] internal_context_options background_indefinite(internal_owner_key owner,
    internal_priority priority) noexcept {
    return internal_context_options{
        .owner = owner,
        .priority = priority,
        .ttl = ttl_permission::finite_or_indefinite,
        .foreground = foreground_policy::background,
    };
}

TEST_CASE("output.concurrent_submit_and_conditional_clear_serialize_same_owner",
    "[component][output]") {
    authority_fixture fixture{};
    auto trusted_a = inputnext::test::output::make_internal_producer(fixture.trusted,
        background_indefinite(internal_owner_key{710U}, internal_priority::game_semantic));
    auto trusted_b = inputnext::test::output::make_internal_producer(fixture.trusted,
        background_indefinite(internal_owner_key{711U}, internal_priority::generic_profile));
    const auto owner_a = trusted_a.open(fixture.connection.target);
    const auto owner_b = trusted_b.open(fixture.connection.target);
    REQUIRE(owner_a.has_value());
    REQUIRE(owner_b.has_value());

    const auto initial = trusted_a.submit(*owner_a,
        paired_request({.sequence = 1U,
            .trigger_strength = 0.25F,
            .low_frequency = 0.2F,
            .high_frequency = 0.3F}));
    const auto untouched = trusted_b.submit(*owner_b, led_request(1U, {10U, 20U, 30U}));
    REQUIRE(initial.has_value());
    REQUIRE(untouched.has_value());

    const auto replacement = paired_request(
        {.sequence = 2U, .trigger_strength = 0.75F, .low_frequency = 0.7F, .high_frequency = 0.8F});
    const clear_request clear{
        .selected_channels = left_trigger_mask | rumble_mask,
        .sequence = 3U,
        .expected_revisions = initial->resulting_revisions,
    };

    controlled_start start;
    auto submit_result = result<mutation_receipt>{std::unexpected{error::resources}};
    auto clear_result = result<mutation_receipt>{std::unexpected{error::resources}};
    std::jthread submitter{[&] {
        if (start.arrive_and_wait()) {
            submit_result = trusted_a.submit(*owner_a, replacement);
        }
    }};
    std::jthread clearer{[&] {
        if (start.arrive_and_wait()) {
            clear_result = trusted_a.clear(*owner_a, clear);
        }
    }};
    const bool both_ready = start.wait_until_ready(2U);
    release_and_join(start, submitter, clearer);
    CHECK(both_ready);
    CHECK(submit_result.has_value() != clear_result.has_value());

    if (clear_result.has_value()) {
        CHECK(clear_result->accepted_sequence == 3U);
        CHECK((!submit_result.has_value() && submit_result.error() == error::sequence_rejected));
    } else {
        REQUIRE(submit_result.has_value());
        CHECK(submit_result->accepted_sequence == 2U);
        CHECK((!clear_result.has_value() && clear_result.error() == error::stale_revision));
    }

    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->leds.has_value());
    const auto led = snapshot->leds.value_or(channel_snapshot<led_value>{});
    CHECK((led.value.rgb == std::array<std::uint8_t, 3U>{10U, 20U, 30U}));
    CHECK(led.provenance.identity == owner_b->identity);
    CHECK(led.provenance.accepted_sequence == 1U);
    if (submit_result.has_value()) {
        CHECK(snapshot->present_channels() == (left_trigger_mask | rumble_mask | leds_mask));
        REQUIRE(snapshot->left_trigger.has_value());
        REQUIRE(snapshot->rumble.has_value());
        const auto trigger = snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{});
        const auto rumble = snapshot->rumble.value_or(channel_snapshot<rumble_value>{});
        CHECK(trigger.provenance.identity == owner_a->identity);
        CHECK(rumble.provenance.identity == owner_a->identity);
        CHECK(trigger.value.strength == 0.75F);
        CHECK((rumble.value == rumble_value{.low_frequency = 0.7F, .high_frequency = 0.8F}));
        CHECK(trigger.provenance.accepted_sequence == 2U);
        CHECK(rumble.provenance.accepted_sequence == 2U);
    } else {
        CHECK(snapshot->present_channels() == leds_mask);
    }
}

TEST_CASE("output.concurrent_close_write_and_snapshots_are_whole_owner_projections",
    "[component][output]") {
    authority_fixture fixture{};
    auto trusted_a = inputnext::test::output::make_internal_producer(fixture.trusted,
        background_indefinite(internal_owner_key{720U}, internal_priority::diagnostic));
    auto trusted_b = inputnext::test::output::make_internal_producer(fixture.trusted,
        background_indefinite(internal_owner_key{721U}, internal_priority::generic_profile));
    const auto owner_a = trusted_a.open(fixture.connection.target);
    const auto owner_b = trusted_b.open(fixture.connection.target);
    REQUIRE(owner_a.has_value());
    REQUIRE(owner_b.has_value());

    const auto old_a = paired_request(
        {.sequence = 1U, .trigger_strength = 0.2F, .low_frequency = 0.2F, .high_frequency = 0.3F});
    const auto old_b = paired_request(
        {.sequence = 1U, .trigger_strength = 0.4F, .low_frequency = 0.4F, .high_frequency = 0.5F});
    const auto new_a = paired_request(
        {.sequence = 2U, .trigger_strength = 0.8F, .low_frequency = 0.8F, .high_frequency = 0.9F});
    const auto initial_a = trusted_a.submit(*owner_a, old_a);
    const auto initial_b = trusted_b.submit(*owner_b, old_b);
    REQUIRE(initial_a.has_value());
    REQUIRE(initial_b.has_value());

    constexpr std::size_t snapshot_count = 8U;
    std::vector<std::optional<result<logical_snapshot>>> snapshots(snapshot_count);
    auto submit_result = result<mutation_receipt>{std::unexpected{error::resources}};
    auto close_result = result<close_receipt>{std::unexpected{error::resources}};
    controlled_start start;
    std::jthread submitter{[&] {
        if (start.arrive_and_wait()) {
            submit_result = trusted_a.submit(*owner_a, new_a);
        }
    }};
    std::jthread closer{[&] {
        if (start.arrive_and_wait()) {
            close_result = trusted_a.close(*owner_a, owner_a->identity.id.value);
        }
    }};
    std::jthread reader{[&] {
        if (start.arrive_and_wait()) {
            for (auto& snapshot : snapshots) {
                snapshot = fixture.owner.read_snapshot(fixture.connection.target);
            }
        }
    }};
    const bool all_ready = start.wait_until_ready(3U);
    release_and_join(start, submitter, closer, reader);
    CHECK(all_ready);

    REQUIRE(close_result.has_value());
    CHECK(close_result->identity == owner_a->identity);
    CHECK(close_result->reason == close_reason::explicit_close);
    CHECK(close_result->logical_revocation_version > initial_a->logical_version);
    CHECK((submit_result.has_value() || submit_result.error() == error::revoked));
    if (submit_result.has_value()) {
        CHECK(close_result->logical_revocation_version > submit_result->logical_version);
    }
    const auto retry = trusted_a.close(*owner_a, owner_a->identity.id.value);
    REQUIRE(retry.has_value());
    CHECK(*retry == *close_result);

    for (const auto& observed : snapshots) {
        REQUIRE(observed.has_value());
        const auto captured =
            observed.value_or(result<logical_snapshot>{std::unexpected{error::resources}});
        REQUIRE(captured.has_value());
        const auto projection = captured.value_or(logical_snapshot{});
        CHECK(projection.present_channels() == (left_trigger_mask | rumble_mask));
        REQUIRE(projection.left_trigger.has_value());
        REQUIRE(projection.rumble.has_value());
        const auto trigger = projection.left_trigger.value_or(channel_snapshot<trigger_value>{});
        const auto rumble = projection.rumble.value_or(channel_snapshot<rumble_value>{});
        CHECK(trigger.provenance.identity == rumble.provenance.identity);
        CHECK(trigger.provenance.accepted_sequence == rumble.provenance.accepted_sequence);
        CHECK(trigger.provenance.revision == rumble.provenance.revision);
        if (trigger.provenance.identity == owner_a->identity) {
            CHECK(projection.logical_version < close_result->logical_revocation_version);
            if (trigger.provenance.accepted_sequence == 1U) {
                CHECK(trigger.value.strength == 0.2F);
                CHECK(rumble.value == old_a.values.rumble);
            } else {
                CHECK(trigger.provenance.accepted_sequence == 2U);
                CHECK(trigger.value.strength == 0.8F);
                CHECK(rumble.value == new_a.values.rumble);
            }
        } else {
            CHECK(trigger.provenance.identity == owner_b->identity);
            CHECK(projection.logical_version >= close_result->logical_revocation_version);
            CHECK(trigger.provenance.accepted_sequence == 1U);
            CHECK(trigger.value.strength == 0.4F);
            CHECK(rumble.value == old_b.values.rumble);
        }
    }

    const auto after_close = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(after_close.has_value());
    REQUIRE(after_close->left_trigger.has_value());
    REQUIRE(after_close->rumble.has_value());
    const auto after_close_trigger =
        after_close->left_trigger.value_or(channel_snapshot<trigger_value>{});
    const auto after_close_rumble = after_close->rumble.value_or(channel_snapshot<rumble_value>{});
    CHECK(after_close_trigger.provenance.identity == owner_b->identity);
    CHECK(after_close_rumble.provenance.identity == owner_b->identity);
    REQUIRE(trusted_a.acknowledge(*close_result).has_value());
}

TEST_CASE("output.concurrent_exact_ack_and_full_public_pool_admission_preserve_slot_ownership",
    "[component][output]") {
    authority_fixture fixture{};
    std::array<public_context, 31U> other_owners{};
    const auto owner_a = fixture.owner.open(fixture.connection.target);
    REQUIRE(owner_a.has_value());
    for (auto& owner : other_owners) {
        const auto opened = fixture.owner.open(fixture.connection.target);
        REQUIRE(opened.has_value());
        owner = *opened;
    }
    const auto closed = fixture.owner.close(*owner_a, owner_a->identity.id.value);
    REQUIRE(closed.has_value());

    controlled_start start;
    auto admission = result<public_context>{std::unexpected{error::resources}};
    auto first_ack = result<void>{std::unexpected{error::resources}};
    auto duplicate_ack = result<void>{std::unexpected{error::resources}};
    std::jthread opener{[&] {
        if (start.arrive_and_wait()) {
            admission = fixture.owner.open(fixture.connection.target);
        }
    }};
    std::jthread acknowledger{[&] {
        if (start.arrive_and_wait()) {
            first_ack = fixture.owner.acknowledge(*closed);
        }
    }};
    std::jthread duplicate_acknowledger{[&] {
        if (start.arrive_and_wait()) {
            duplicate_ack = fixture.owner.acknowledge(*closed);
        }
    }};
    const bool all_ready = start.wait_until_ready(3U);
    release_and_join(start, opener, acknowledger, duplicate_acknowledger);
    CHECK(all_ready);

    const auto successful_acks = static_cast<std::size_t>(first_ack.has_value()) +
                                 static_cast<std::size_t>(duplicate_ack.has_value());
    CHECK(successful_acks == 1U);
    const auto& rejected_ack = first_ack.has_value() ? duplicate_ack : first_ack;
    CHECK((!rejected_ack.has_value() && rejected_ack.error() == error::stale_context));

    std::optional<public_context> admitted;
    if (admission.has_value()) {
        admitted = *admission;
    } else {
        CHECK(admission.error() == error::capacity_exceeded);
        const auto retried = fixture.owner.open(fixture.connection.target);
        REQUIRE(retried.has_value());
        admitted = *retried;
    }
    REQUIRE(admitted.has_value());
    CHECK(admitted->identity.id.value > owner_a->identity.id.value);

    const auto stale_ack = fixture.owner.acknowledge(*closed);
    CHECK((!stale_ack.has_value() && stale_ack.error() == error::stale_context));
    const auto still_full = fixture.owner.open(fixture.connection.target);
    CHECK((!still_full.has_value() && still_full.error() == error::capacity_exceeded));
    for (const auto& owner : other_owners) {
        const auto info = fixture.owner.inspect(owner);
        REQUIRE(info.has_value());
        CHECK(info->identity == owner.identity);
        CHECK(info->accepted_sequence == 0U);
    }
    const auto stale_owner = fixture.owner.inspect(*owner_a);
    CHECK((!stale_owner.has_value() && stale_owner.error() == error::stale_context));
}

TEST_CASE("output.concurrent_mutations_have_a_coherent_snapshot_order", "[component][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());

    std::mutex gate;
    std::condition_variable changed;
    std::size_t arrived = 0U;
    bool start = false;
    auto first_result =
        inputnext::output::result<mutation_receipt>{std::unexpected{output_error::resources}};
    auto second_result =
        inputnext::output::result<mutation_receipt>{std::unexpected{output_error::resources}};
    const auto run = [&](std::uint64_t sequence,
                         channel_mask selected,
                         inputnext::output::result<mutation_receipt>& result) {
        {
            std::unique_lock lock{gate};
            ++arrived;
            changed.notify_all();
            if (!changed.wait_for(lock, 2s, [&start] { return start; })) {
                return;
            }
        }
        const auto request = inputnext::test::output::rumble_request(
            {.channels = selected, .sequence = sequence, .ttl = 5s, .low = 0.3F, .high = 0.4F});
        result = fixture.owner.submit(*opened, request);
    };

    std::jthread first{[&] { run(1U, rumble_mask, first_result); }};
    std::jthread second{[&] { run(2U, left_trigger_mask, second_result); }};
    bool both_arrived = false;
    {
        std::unique_lock lock{gate};
        both_arrived = changed.wait_for(lock, 2s, [&arrived] { return arrived == 2U; });
        start = true;
        changed.notify_all();
    }
    first.join();
    second.join();
    CHECK(both_arrived);
    REQUIRE(second_result.has_value());

    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 2U);
    const auto snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(snapshot.has_value());
    CHECK((snapshot->present_channels() & left_trigger_mask) != 0U);
    REQUIRE(snapshot->left_trigger.has_value());
    CHECK(snapshot->left_trigger.value_or(channel_snapshot<trigger_value>{})
              .provenance.accepted_sequence == 2U);
    if (first_result.has_value()) {
        CHECK((snapshot->present_channels() & rumble_mask) != 0U);
        CHECK(snapshot->rumble.value_or(channel_snapshot<rumble_value>{})
                  .provenance.accepted_sequence == 1U);
    } else {
        CHECK(first_result.error() == output_error::sequence_rejected);
        CHECK((snapshot->present_channels() & rumble_mask) == 0U);
    }
}

} // namespace
