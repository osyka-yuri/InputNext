#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/controlled_start.hpp"
#include <inputnext/output.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using inputnext::test::output::controlled_start;
using output_error = inputnext::output::error;

TEST_CASE("output.public_handles_require_full_exact_identity_and_class", "[component][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());

    auto foreign_runtime = *opened;
    foreign_runtime.identity.target.runtime.bytes[1U] = std::byte{1U};
    auto wrong_class = *opened;
    wrong_class.identity.kind = owner_class::trusted_internal;
    wrong_class.identity.internal_owner = internal_owner_key{5U};
    auto wrong_id = *opened;
    ++wrong_id.identity.id.value;
    const auto request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.5F, .high = 0.5F});

    const auto foreign_result = fixture.owner.submit(foreign_runtime, request);
    const auto class_result = fixture.owner.submit(wrong_class, request);
    const auto id_result = fixture.owner.submit(wrong_id, request);
    CHECK((!foreign_result.has_value() && foreign_result.error() == output_error::stale_context));
    CHECK((!class_result.has_value() && class_result.error() == output_error::stale_context));
    CHECK((!id_result.has_value() && id_result.error() == output_error::stale_context));

    const internal_context_options internal_options{
        .owner = internal_owner_key{600U},
        .priority = internal_priority::game_profile,
        .ttl = ttl_permission::finite_or_indefinite,
        .foreground = foreground_policy::background,
    };
    auto trusted =
        inputnext::test::output::make_internal_producer(fixture.trusted, internal_options);
    const auto internal = trusted.open(fixture.connection.target);
    REQUIRE(internal.has_value());

    const auto public_seed = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.2F, .high = 0.3F}));
    const auto internal_seed = trusted.submit(*internal,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 5s, .low = 0.4F, .high = 0.5F}));
    REQUIRE(public_seed.has_value());
    REQUIRE(internal_seed.has_value());

    const public_context internal_identity_as_public{internal->identity};
    const internal_context public_identity_as_internal{opened->identity};
    const auto submit_internal_as_public = fixture.owner.submit(internal_identity_as_public,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.6F, .high = 0.7F}));
    const clear_request clear_internal_as_public{
        .selected_channels = rumble_mask,
        .sequence = 3U,
        .expected_revisions = internal_seed->resulting_revisions,
    };
    const auto cleared_internal_as_public =
        fixture.owner.clear(internal_identity_as_public, clear_internal_as_public);
    const auto inspected_internal_as_public = fixture.owner.inspect(internal_identity_as_public);
    const auto closed_internal_as_public = fixture.owner.close(internal_identity_as_public,
        internal_identity_as_public.identity.id.value);
    CHECK((!submit_internal_as_public.has_value() &&
           submit_internal_as_public.error() == output_error::stale_context));
    CHECK((!cleared_internal_as_public.has_value() &&
           cleared_internal_as_public.error() == output_error::stale_context));
    CHECK((!inspected_internal_as_public.has_value() &&
           inspected_internal_as_public.error() == output_error::stale_context));
    CHECK((!closed_internal_as_public.has_value() &&
           closed_internal_as_public.error() == output_error::stale_context));

    const auto submit_public_as_internal = trusted.submit(public_identity_as_internal,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.6F, .high = 0.7F}));
    const clear_request clear_public_as_internal{
        .selected_channels = rumble_mask,
        .sequence = 3U,
        .expected_revisions = public_seed->resulting_revisions,
    };
    const auto cleared_public_as_internal =
        trusted.clear(public_identity_as_internal, clear_public_as_internal);
    const auto closed_public_as_internal =
        trusted.close(public_identity_as_internal, public_identity_as_internal.identity.id.value);
    CHECK((!submit_public_as_internal.has_value() &&
           submit_public_as_internal.error() == output_error::stale_context));
    CHECK((!cleared_public_as_internal.has_value() &&
           cleared_public_as_internal.error() == output_error::stale_context));
    CHECK((!closed_public_as_internal.has_value() &&
           closed_public_as_internal.error() == output_error::stale_context));

    const auto info = fixture.owner.inspect(*opened);
    REQUIRE(info.has_value());
    CHECK(info->accepted_sequence == 1U);
    CHECK(info->revisions == public_seed->resulting_revisions);
    const auto internal_followup = trusted.submit(*internal,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.6F, .high = 0.7F}));
    REQUIRE(internal_followup.has_value());
    CHECK(internal_followup->resulting_revisions[2U] == 2U);
}

TEST_CASE("output.authority_incarnations_are_unique_and_creation_failure_is_closed",
    "[component][output]") {
    authority_fixture fixture{};
    std::optional<authority> reconstructed;
    reconstructed.emplace(fixture.runtime, fixture.registry, fixture.focus_observer);
    const auto* const storage = std::addressof(*reconstructed);
    const auto first = reconstructed->open(fixture.connection.target);
    REQUIRE(first.has_value());
    const auto first_incarnation = first->identity.incarnation;
    CHECK(first_incarnation.valid());
    const auto first_receipt = reconstructed->close(*first, first->identity.id.value);
    REQUIRE(first_receipt.has_value());
    reconstructed.reset();

    reconstructed.emplace(fixture.runtime, fixture.registry, fixture.focus_observer);
    CHECK(std::addressof(*reconstructed) == storage);
    const auto second = reconstructed->open(fixture.connection.target);
    REQUIRE(second.has_value());
    CHECK(second->identity.id == first->identity.id);
    CHECK(second->identity.target == first->identity.target);
    CHECK(second->identity.incarnation != first_incarnation);
    const auto second_receipt = reconstructed->close(*second, second->identity.id.value);
    REQUIRE(second_receipt.has_value());
    CHECK(second_receipt->close_request_id == first_receipt->close_request_id);
    CHECK(second_receipt->logical_revocation_version == first_receipt->logical_revocation_version);
    auto first_receipt_with_current_incarnation = *first_receipt;
    first_receipt_with_current_incarnation.identity.incarnation = second->identity.incarnation;
    CHECK(first_receipt_with_current_incarnation == *second_receipt);

    const auto stale_reconstruction = reconstructed->submit(*first,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1s}));
    CHECK((!stale_reconstruction.has_value() &&
           stale_reconstruction.error() == output_error::stale_context));
    const auto stale_receipt_ack = reconstructed->acknowledge(*first_receipt);
    CHECK((!stale_receipt_ack.has_value() &&
           stale_receipt_ack.error() == output_error::stale_context));
    const auto retained_reconstructed_record =
        inputnext::output::detail::authority_test_access::observe_record(*reconstructed,
            second->identity);
    REQUIRE(retained_reconstructed_record.has_value());
    const auto retained_reconstructed_state = retained_reconstructed_record.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(retained_reconstructed_state.receipt == *second_receipt);
    constexpr std::size_t concurrent_count = 12U;
    std::array<authority_incarnation, concurrent_count> concurrent_incarnations{};
    std::atomic<bool> construction_failed{};
    controlled_start construction_start;
    std::array<std::jthread, concurrent_count> constructors{};
    const auto release_and_join_constructors = [&] {
        construction_start.release();
        for (auto& constructor : constructors) {
            if (constructor.joinable()) {
                constructor.join();
            }
        }
    };
    try {
        for (std::size_t index = 0U; index < concurrent_count; ++index) {
            constructors[index] = std::jthread{[&, index] {
                if (construction_start.arrive_and_wait()) {
                    try {
                        const authority concurrent{fixture.runtime,
                            fixture.registry,
                            fixture.focus_observer};
                        concurrent_incarnations[index] =
                            inputnext::output::detail::authority_test_access::incarnation(
                                concurrent);
                    } catch (...) {
                        construction_failed.store(true, std::memory_order_release);
                    }
                }
            }};
        }
    } catch (...) {
        release_and_join_constructors();
        throw;
    }
    const auto all_ready = construction_start.wait_until_ready(concurrent_count);
    release_and_join_constructors();
    CHECK(all_ready);
    CHECK(!construction_failed.load(std::memory_order_acquire));
    for (std::size_t first_index = 0U; first_index < concurrent_count; ++first_index) {
        const auto first_value = concurrent_incarnations[first_index];
        CHECK(first_value.valid());
        for (std::size_t second_index = first_index + 1U; second_index < concurrent_count;
            ++second_index) {
            CHECK(first_value != concurrent_incarnations[second_index]);
        }
    }

    CHECK_THROWS_AS(
        static_cast<void>(
            inputnext::output::detail::authority_test_access::make_with_generation_failure(
                fixture.runtime,
                fixture.registry,
                fixture.focus_observer)),
        std::runtime_error);
    CHECK_THROWS_AS(
        static_cast<void>(
            inputnext::output::detail::authority_test_access::make_with_generation_failure(
                runtime_id{},
                fixture.registry,
                fixture.focus_observer)),
        std::invalid_argument);
}

TEST_CASE("output.foreign_incarnation_is_fenced_before_maintenance_and_ack",
    "[component][output]") {
    authority_fixture fixture{};
    authority foreign{fixture.runtime, fixture.registry, fixture.focus_observer};
    std::atomic<std::int64_t> ticks{};
    inputnext::output::detail::authority_test_access::use_clock(fixture.owner, ticks);
    inputnext::output::detail::authority_test_access::use_clock(foreign, ticks);

    const auto victim_context = fixture.owner.open(fixture.connection.target);
    const auto foreign_context = foreign.open(fixture.connection.target);
    REQUIRE(victim_context.has_value());
    REQUIRE(foreign_context.has_value());
    CHECK(victim_context->identity.id == foreign_context->identity.id);
    CHECK(victim_context->identity.target == foreign_context->identity.target);
    CHECK(victim_context->identity.incarnation != foreign_context->identity.incarnation);

    const auto same_history_request = inputnext::test::output::rumble_request(
        {.channels = rumble_mask, .sequence = 1U, .ttl = 60s, .low = 0.4F, .high = 0.6F});
    const auto victim_mutation = fixture.owner.submit(*victim_context, same_history_request);
    const auto foreign_mutation = foreign.submit(*foreign_context, same_history_request);
    REQUIRE(victim_mutation.has_value());
    REQUIRE(foreign_mutation.has_value());
    CHECK(victim_mutation->accepted_sequence == foreign_mutation->accepted_sequence);
    CHECK(victim_mutation->resulting_revisions == foreign_mutation->resulting_revisions);
    CHECK(victim_mutation->logical_version == foreign_mutation->logical_version);
    CHECK(victim_mutation->identity.incarnation != foreign_mutation->identity.incarnation);

    const auto victim_close =
        fixture.owner.close(*victim_context, victim_context->identity.id.value);
    const auto foreign_close = foreign.close(*foreign_context, foreign_context->identity.id.value);
    REQUIRE(victim_close.has_value());
    REQUIRE(foreign_close.has_value());
    CHECK(victim_close->close_request_id == foreign_close->close_request_id);
    CHECK(victim_close->logical_revocation_version == foreign_close->logical_revocation_version);
    auto equal_history_receipt = *foreign_close;
    equal_history_receipt.identity.incarnation = victim_close->identity.incarnation;
    CHECK(equal_history_receipt == *victim_close);
    const auto foreign_ack_at_victim = fixture.owner.acknowledge(*foreign_close);
    const auto victim_ack_at_foreign = foreign.acknowledge(*victim_close);
    CHECK((!foreign_ack_at_victim.has_value() &&
           foreign_ack_at_victim.error() == output_error::stale_context));
    CHECK((!victim_ack_at_foreign.has_value() &&
           victim_ack_at_foreign.error() == output_error::stale_context));
    const auto retained_victim =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_context->identity);
    REQUIRE(retained_victim.has_value());
    const auto retained_victim_state = retained_victim.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(!retained_victim_state.open);
    CHECK(retained_victim_state.receipt == *victim_close);

    const auto victim_due_context = fixture.owner.open(fixture.connection.target);
    const auto foreign_due_context = foreign.open(fixture.connection.target);
    REQUIRE(victim_due_context.has_value());
    REQUIRE(foreign_due_context.has_value());
    CHECK(victim_due_context->identity.id == foreign_due_context->identity.id);
    const auto due_submit = fixture.owner.submit(*victim_due_context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1ms, .low = 0.7F, .high = 0.8F}));
    REQUIRE(due_submit.has_value());
    for (std::size_t index = 0U; index < 30U; ++index) {
        const auto opened = fixture.owner.open(fixture.connection.target);
        REQUIRE(opened.has_value());
    }

    ticks.store(std::chrono::duration_cast<std::chrono::steady_clock::duration>(1ms).count(),
        std::memory_order_release);
    const auto before_foreign_calls =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_due_context->identity);
    REQUIRE(before_foreign_calls.has_value());
    const auto before_state = before_foreign_calls.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(before_state.open);
    CHECK(before_state.accepted_sequence == 1U);
    CHECK(before_state.active[2U]);

    const auto foreign_submit_at_victim = fixture.owner.submit(*foreign_due_context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.9F, .high = 0.95F}));
    const clear_request foreign_clear{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = due_submit->resulting_revisions,
    };
    const auto foreign_clear_at_victim = fixture.owner.clear(*foreign_due_context, foreign_clear);
    const auto foreign_inspect_at_victim = fixture.owner.inspect(*foreign_due_context);
    const auto foreign_close_at_victim =
        fixture.owner.close(*foreign_due_context, foreign_due_context->identity.id.value);
    CHECK((!foreign_submit_at_victim.has_value() &&
           foreign_submit_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_clear_at_victim.has_value() &&
           foreign_clear_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_inspect_at_victim.has_value() &&
           foreign_inspect_at_victim.error() == output_error::stale_context));
    CHECK((!foreign_close_at_victim.has_value() &&
           foreign_close_at_victim.error() == output_error::stale_context));

    const auto after_foreign_calls =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_due_context->identity);
    REQUIRE(after_foreign_calls.has_value());
    const auto after_state = after_foreign_calls.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(after_state.open);
    CHECK(after_state.accepted_sequence == before_state.accepted_sequence);
    CHECK(after_state.revisions == before_state.revisions);
    CHECK(after_state.active == before_state.active);
    CHECK(after_state.logical_version == before_state.logical_version);

    const auto capacity_after_foreign_ack = fixture.owner.open(fixture.connection.target);
    CHECK((!capacity_after_foreign_ack.has_value() &&
           capacity_after_foreign_ack.error() == output_error::capacity_exceeded));
    const auto retained_after_capacity =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_context->identity);
    REQUIRE(retained_after_capacity.has_value());
    const auto retained_after_capacity_state = retained_after_capacity.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(retained_after_capacity_state.receipt == *victim_close);

    fixture.focus_observer.stop();
    const auto version_before_pending_focus =
        inputnext::output::detail::authority_test_access::logical_version(fixture.owner);
    const auto pending_focus_submit = fixture.owner.submit(*foreign_due_context,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 2U, .ttl = 5s, .low = 0.9F, .high = 0.95F}));
    const clear_request pending_focus_clear{
        .selected_channels = rumble_mask,
        .sequence = 2U,
        .expected_revisions = due_submit->resulting_revisions,
    };
    const auto pending_focus_clear_result =
        fixture.owner.clear(*foreign_due_context, pending_focus_clear);
    const auto pending_focus_inspect = fixture.owner.inspect(*foreign_due_context);
    const auto pending_focus_close =
        fixture.owner.close(*foreign_due_context, foreign_due_context->identity.id.value);
    CHECK((!pending_focus_submit.has_value() &&
           pending_focus_submit.error() == output_error::stale_context));
    CHECK((!pending_focus_clear_result.has_value() &&
           pending_focus_clear_result.error() == output_error::stale_context));
    CHECK((!pending_focus_inspect.has_value() &&
           pending_focus_inspect.error() == output_error::stale_context));
    CHECK((!pending_focus_close.has_value() &&
           pending_focus_close.error() == output_error::stale_context));
    const auto pending_focus_record =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            victim_due_context->identity);
    REQUIRE(pending_focus_record.has_value());
    const auto pending_focus_state = pending_focus_record.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(pending_focus_state.open);
    CHECK(pending_focus_state.logical_version == version_before_pending_focus);
}

} // namespace
