#include <inputnext/output_authority.hpp>

#include "output/authority_fixture.hpp"
#include "output/authority_test_access.hpp"
#include "output/composition/composition.hpp"
#include <inputnext/output.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::output;
using inputnext::test::output::authority_fixture;
using output_error = inputnext::output::error;

TEST_CASE("output.authority_receipt_retry_and_exact_ack", "[component][output]") {
    authority_fixture fixture{};
    const auto opened = fixture.owner.open(fixture.connection.target);
    REQUIRE(opened.has_value());
    const auto closed = fixture.owner.close(*opened, opened->identity.id.value);
    REQUIRE(closed.has_value());
    CHECK(closed->identity == opened->identity);
    CHECK(closed->close_request_id == opened->identity.id);
    CHECK(closed->logical_revocation_version > 0U);
    CHECK(closed->reason == close_reason::explicit_close);
    const auto closed_inspection = fixture.owner.inspect(*opened);
    CHECK((!closed_inspection.has_value() && closed_inspection.error() == output_error::revoked));

    const auto retry = fixture.owner.close(*opened, opened->identity.id.value);
    REQUIRE(retry.has_value());
    CHECK(*retry == *closed);
    const auto revoked = fixture.owner.submit(*opened,
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1s, .low = 1.0F, .high = 1.0F}));
    CHECK((!revoked.has_value() && revoked.error() == output_error::revoked));

    const auto reject_altered_receipt = [&fixture](close_receipt altered) {
        const auto result = fixture.owner.acknowledge(altered);
        CHECK((!result.has_value() && result.error() == output_error::stale_context));
    };
    auto altered = *closed;
    altered.identity.target.runtime.bytes[1U] = std::byte{1U};
    reject_altered_receipt(altered);
    altered = *closed;
    ++altered.identity.target.device.value;
    reject_altered_receipt(altered);
    altered = *closed;
    ++altered.identity.target.connection.value;
    reject_altered_receipt(altered);
    altered = *closed;
    ++altered.identity.id.value;
    reject_altered_receipt(altered);
    altered = *closed;
    altered.identity.kind = owner_class::trusted_internal;
    altered.identity.internal_owner = internal_owner_key{1U};
    reject_altered_receipt(altered);
    altered = *closed;
    altered.identity.internal_owner = internal_owner_key{1U};
    reject_altered_receipt(altered);
    altered = *closed;
    altered.identity.incarnation.bytes[0U] =
        altered.identity.incarnation.bytes[0U] == std::byte{1U} ? std::byte{2U} : std::byte{1U};
    reject_altered_receipt(altered);
    altered = *closed;
    ++altered.close_request_id.value;
    reject_altered_receipt(altered);
    altered = *closed;
    ++altered.logical_revocation_version;
    reject_altered_receipt(altered);
    altered = *closed;
    altered.reason = close_reason::foreground_revocation;
    reject_altered_receipt(altered);
    REQUIRE(fixture.owner.acknowledge(*closed).has_value());
    const auto duplicate_ack = fixture.owner.acknowledge(*closed);
    CHECK((!duplicate_ack.has_value() && duplicate_ack.error() == output_error::stale_context));
    const auto reclaimed_inspection = fixture.owner.inspect(*opened);
    CHECK((!reclaimed_inspection.has_value() &&
           reclaimed_inspection.error() == output_error::stale_context));

    const auto replacement = fixture.owner.open(fixture.connection.target);
    REQUIRE(replacement.has_value());
    CHECK(replacement->identity.id.value > opened->identity.id.value);
    const auto stale_ack = fixture.owner.acknowledge(*closed);
    CHECK((!stale_ack.has_value() && stale_ack.error() == output_error::stale_context));
}

TEST_CASE("output.inspect_reports_stored_state_until_maintenance_requalifies_liveness",
    "[component][output]") {
    authority_fixture fixture{};
    auto second_source = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto second_connection = fixture.registry.admit(second_source);
    REQUIRE(second_connection.has_value());
    const auto focus_bound = fixture.owner.open(fixture.connection.target);
    const auto retired = fixture.owner.open(second_connection->target);
    REQUIRE(focus_bound.has_value());
    REQUIRE(retired.has_value());
    const auto stored_epoch = fixture.owner.inspect(*focus_bound);
    REQUIRE(stored_epoch.has_value());
    REQUIRE(stored_epoch->foreground.has_value());

    fixture.focus_observer.stop();
    const auto after_stop = fixture.owner.inspect(*focus_bound);
    REQUIRE(after_stop.has_value());
    CHECK(after_stop->identity == focus_bound->identity);
    CHECK(after_stop->foreground == stored_epoch->foreground);
    const auto stopped_record =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            focus_bound->identity);
    REQUIRE(stopped_record.has_value());
    const auto stopped_state = stopped_record.value_or(
        inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(stopped_state.open);

    const auto retired_registry = fixture.registry.retire(second_connection->target);
    REQUIRE(retired_registry.has_value());
    const auto before_retirement_maintenance = fixture.owner.inspect(*retired);
    REQUIRE(before_retirement_maintenance.has_value());
    CHECK(before_retirement_maintenance->identity == retired->identity);
    const auto still_open =
        inputnext::output::detail::authority_test_access::observe_record(fixture.owner,
            retired->identity);
    REQUIRE(still_open.has_value());
    const auto still_open_state =
        still_open.value_or(inputnext::output::detail::authority_test_access::record_observation{});
    CHECK(still_open_state.open);

    const auto stopped_snapshot = fixture.owner.read_snapshot(fixture.connection.target);
    REQUIRE(stopped_snapshot.has_value());
    CHECK(stopped_snapshot->present_channels() == 0U);
    const auto revoked_after_snapshot = fixture.owner.inspect(*focus_bound);
    CHECK((!revoked_after_snapshot.has_value() &&
           revoked_after_snapshot.error() == output_error::revoked));

    const auto retirement = fixture.owner.maintain(second_connection->target);
    REQUIRE(retirement.has_value());
    CHECK(retirement->reclaimed_contexts >= 1U);
    const auto reclaimed_inspection = fixture.owner.inspect(*retired);
    CHECK((!reclaimed_inspection.has_value() &&
           reclaimed_inspection.error() == output_error::stale_context));
}

TEST_CASE("output.authority_retained_bounds_and_internal_dedup", "[component][output]") {
    authority_fixture fixture{};
    inputnext::output::detail::composition foreign_root{fixture.runtime,
        fixture.registry,
        fixture.focus_observer};
    auto& foreign_owner = foreign_root.public_authority();

    std::vector<public_context> public_contexts(32U);
    std::vector<public_context> foreign_public_contexts(32U);
    std::vector<close_receipt> public_receipts(32U);
    std::vector<close_receipt> foreign_public_receipts(32U);
    for (std::size_t index = 0U; index < public_contexts.size(); ++index) {
        const auto opened = fixture.owner.open(fixture.connection.target);
        const auto foreign_opened = foreign_owner.open(fixture.connection.target);
        REQUIRE(opened.has_value());
        REQUIRE(foreign_opened.has_value());
        CHECK(opened->identity.id == foreign_opened->identity.id);
        CHECK(opened->identity.target == foreign_opened->identity.target);
        CHECK(opened->identity.incarnation != foreign_opened->identity.incarnation);
        public_contexts[index] = *opened;
        foreign_public_contexts[index] = *foreign_opened;
    }

    std::vector<internal_context> internal_contexts(32U);
    std::vector<internal_context> foreign_internal_contexts(32U);
    std::vector<close_receipt> internal_receipts(32U);
    std::vector<close_receipt> foreign_internal_receipts(32U);
    std::vector<inputnext::output::detail::internal_producer> internal_producers;
    std::vector<inputnext::output::detail::internal_producer> foreign_internal_producers;
    internal_producers.reserve(internal_contexts.size());
    foreign_internal_producers.reserve(foreign_internal_contexts.size());
    for (std::size_t index = 0U; index < internal_contexts.size(); ++index) {
        const auto key = internal_owner_key{static_cast<std::uint64_t>(index + 1U)};
        const internal_context_options options{
            .owner = key,
            .priority = internal_priority::generic_profile,
            .ttl = ttl_permission::finite_only,
            .foreground = foreground_policy::background,
        };
        internal_producers.emplace_back(
            inputnext::test::output::make_internal_producer(fixture.trusted, options));
        foreign_internal_producers.emplace_back(
            inputnext::test::output::make_internal_producer(foreign_root, options));
        auto& trusted = internal_producers.back();
        auto& foreign_trusted = foreign_internal_producers.back();
        const auto opened = trusted.open(fixture.connection.target);
        const auto foreign_opened = foreign_trusted.open(fixture.connection.target);
        REQUIRE(opened.has_value());
        REQUIRE(foreign_opened.has_value());
        CHECK(opened->identity.id == foreign_opened->identity.id);
        CHECK(opened->identity.target == foreign_opened->identity.target);
        CHECK(opened->identity.incarnation != foreign_opened->identity.incarnation);
        internal_contexts[index] = *opened;
        foreign_internal_contexts[index] = *foreign_opened;
        if (index == 0U) {
            const auto duplicate = trusted.open(fixture.connection.target);
            REQUIRE(duplicate.has_value());
            CHECK(duplicate->identity == opened->identity);
            const auto foreign_duplicate = foreign_trusted.open(fixture.connection.target);
            REQUIRE(foreign_duplicate.has_value());
            CHECK(foreign_duplicate->identity == foreign_opened->identity);
            auto changed = options;
            changed.priority = internal_priority::game_profile;
            auto changed_producer =
                inputnext::test::output::make_internal_producer(fixture.trusted, changed);
            const auto mismatch = changed_producer.open(fixture.connection.target);
            CHECK((!mismatch.has_value() && mismatch.error() == output_error::invalid_argument));
        }
    }

    const internal_context_options overflow{
        .owner = internal_owner_key{33U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto overflow_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, overflow);
    auto foreign_overflow_producer =
        inputnext::test::output::make_internal_producer(foreign_root, overflow);
    const auto overflow_result = overflow_producer.open(fixture.connection.target);
    const auto foreign_overflow_result = foreign_overflow_producer.open(fixture.connection.target);
    CHECK((!overflow_result.has_value() &&
           overflow_result.error() == output_error::capacity_exceeded));
    CHECK((!foreign_overflow_result.has_value() &&
           foreign_overflow_result.error() == output_error::capacity_exceeded));

    for (std::size_t index = 0U; index < public_contexts.size(); ++index) {
        const auto receipt =
            fixture.owner.close(public_contexts[index], public_contexts[index].identity.id.value);
        const auto foreign_receipt = foreign_owner.close(foreign_public_contexts[index],
            foreign_public_contexts[index].identity.id.value);
        REQUIRE(receipt.has_value());
        REQUIRE(foreign_receipt.has_value());
        public_receipts[index] = *receipt;
        foreign_public_receipts[index] = *foreign_receipt;
    }
    auto normalized_public_receipt = foreign_public_receipts[0U];
    normalized_public_receipt.identity.incarnation = public_receipts[0U].identity.incarnation;
    CHECK(normalized_public_receipt == public_receipts[0U]);
    const auto foreign_public_ack_at_victim =
        fixture.owner.acknowledge(foreign_public_receipts[0U]);
    const auto victim_public_ack_at_foreign = foreign_owner.acknowledge(public_receipts[0U]);
    const auto producer_ack_of_public = internal_producers[0U].acknowledge(public_receipts[0U]);
    CHECK((!foreign_public_ack_at_victim.has_value() &&
           foreign_public_ack_at_victim.error() == output_error::stale_context));
    CHECK((!victim_public_ack_at_foreign.has_value() &&
           victim_public_ack_at_foreign.error() == output_error::stale_context));
    CHECK((!producer_ack_of_public.has_value() &&
           producer_ack_of_public.error() == output_error::stale_context));
    const auto public_capacity_before_ack = fixture.owner.open(fixture.connection.target);
    CHECK((!public_capacity_before_ack.has_value() &&
           public_capacity_before_ack.error() == output_error::capacity_exceeded));
    const auto foreign_public_capacity_before_ack = foreign_owner.open(fixture.connection.target);
    CHECK((!foreign_public_capacity_before_ack.has_value() &&
           foreign_public_capacity_before_ack.error() == output_error::capacity_exceeded));

    REQUIRE(fixture.owner.acknowledge(public_receipts[0U]).has_value());
    const auto recycled_slot = fixture.owner.open(fixture.connection.target);
    REQUIRE(recycled_slot.has_value());
    CHECK(recycled_slot->identity.id.value > public_contexts.back().identity.id.value);
    const auto duplicate_ack = fixture.owner.acknowledge(public_receipts[0U]);
    CHECK((!duplicate_ack.has_value() && duplicate_ack.error() == output_error::stale_context));
    const auto still_full_public = fixture.owner.open(fixture.connection.target);
    CHECK((!still_full_public.has_value() &&
           still_full_public.error() == output_error::capacity_exceeded));
    REQUIRE(foreign_owner.acknowledge(foreign_public_receipts[0U]).has_value());
    const auto foreign_recycled_slot = foreign_owner.open(fixture.connection.target);
    REQUIRE(foreign_recycled_slot.has_value());
    CHECK(foreign_recycled_slot->identity.id == recycled_slot->identity.id);
    const auto duplicate_foreign_public_ack =
        foreign_owner.acknowledge(foreign_public_receipts[0U]);
    CHECK((!duplicate_foreign_public_ack.has_value() &&
           duplicate_foreign_public_ack.error() == output_error::stale_context));
    const auto still_full_foreign_public = foreign_owner.open(fixture.connection.target);
    CHECK((!still_full_foreign_public.has_value() &&
           still_full_foreign_public.error() == output_error::capacity_exceeded));

    for (std::size_t index = 0U; index < internal_contexts.size(); ++index) {
        const auto receipt = internal_producers[index].close(internal_contexts[index],
            internal_contexts[index].identity.id.value);
        const auto foreign_receipt =
            foreign_internal_producers[index].close(foreign_internal_contexts[index],
                foreign_internal_contexts[index].identity.id.value);
        REQUIRE(receipt.has_value());
        REQUIRE(foreign_receipt.has_value());
        internal_receipts[index] = *receipt;
        foreign_internal_receipts[index] = *foreign_receipt;
    }
    auto normalized_internal_receipt = foreign_internal_receipts[0U];
    normalized_internal_receipt.identity.incarnation = internal_receipts[0U].identity.incarnation;
    CHECK(normalized_internal_receipt == internal_receipts[0U]);
    const auto public_ack_of_internal = fixture.owner.acknowledge(internal_receipts[0U]);
    const auto foreign_ack_at_victim =
        internal_producers[0U].acknowledge(foreign_internal_receipts[0U]);
    const auto victim_ack_at_foreign =
        foreign_internal_producers[0U].acknowledge(internal_receipts[0U]);
    const auto wrong_key_ack = internal_producers[1U].acknowledge(internal_receipts[0U]);
    CHECK((!public_ack_of_internal.has_value() &&
           public_ack_of_internal.error() == output_error::stale_context));
    CHECK((!foreign_ack_at_victim.has_value() &&
           foreign_ack_at_victim.error() == output_error::stale_context));
    CHECK((!victim_ack_at_foreign.has_value() &&
           victim_ack_at_foreign.error() == output_error::stale_context));
    CHECK((!wrong_key_ack.has_value() && wrong_key_ack.error() == output_error::stale_context));
    const auto internal_capacity_before_ack = overflow_producer.open(fixture.connection.target);
    const auto foreign_internal_capacity_before_ack =
        foreign_overflow_producer.open(fixture.connection.target);
    CHECK((!internal_capacity_before_ack.has_value() &&
           internal_capacity_before_ack.error() == output_error::capacity_exceeded));
    CHECK((!foreign_internal_capacity_before_ack.has_value() &&
           foreign_internal_capacity_before_ack.error() == output_error::capacity_exceeded));

    REQUIRE(internal_producers[0U].acknowledge(internal_receipts[0U]).has_value());
    const auto duplicate_internal_ack = internal_producers[0U].acknowledge(internal_receipts[0U]);
    CHECK((!duplicate_internal_ack.has_value() &&
           duplicate_internal_ack.error() == output_error::stale_context));
    const auto recycled_internal = overflow_producer.open(fixture.connection.target);
    REQUIRE(recycled_internal.has_value());
    CHECK(recycled_internal->identity.id.value > internal_contexts.back().identity.id.value);
    REQUIRE(foreign_internal_producers[0U].acknowledge(foreign_internal_receipts[0U]).has_value());
    const auto foreign_recycled_internal =
        foreign_overflow_producer.open(fixture.connection.target);
    REQUIRE(foreign_recycled_internal.has_value());
    CHECK(foreign_recycled_internal->identity.id == recycled_internal->identity.id);
    const internal_context_options next_overflow{
        .owner = internal_owner_key{34U},
        .priority = internal_priority::generic_profile,
        .ttl = ttl_permission::finite_only,
        .foreground = foreground_policy::background,
    };
    auto next_overflow_producer =
        inputnext::test::output::make_internal_producer(fixture.trusted, next_overflow);
    const auto still_full_internal = next_overflow_producer.open(fixture.connection.target);
    CHECK((!still_full_internal.has_value() &&
           still_full_internal.error() == output_error::capacity_exceeded));
    auto foreign_next_overflow_producer =
        inputnext::test::output::make_internal_producer(foreign_root, next_overflow);
    const auto still_full_foreign_internal =
        foreign_next_overflow_producer.open(fixture.connection.target);
    CHECK((!still_full_foreign_internal.has_value() &&
           still_full_foreign_internal.error() == output_error::capacity_exceeded));

    const auto retired = fixture.registry.retire(fixture.connection.target);
    REQUIRE(retired.has_value());
    const auto report = fixture.owner.maintain(fixture.connection.target);
    REQUIRE(report.has_value());
    CHECK(report->reclaimed_contexts >= 1U);
    const auto retired_context = fixture.owner.inspect(*recycled_slot);
    CHECK((!retired_context.has_value() && retired_context.error() == output_error::stale_context));
    const auto stale_receipt_after_retirement = fixture.owner.acknowledge(public_receipts[1U]);
    CHECK((!stale_receipt_after_retirement.has_value() &&
           stale_receipt_after_retirement.error() == output_error::stale_context));

    auto replacement_source = std::unique_ptr<source_lifetime>{
        std::make_unique<inputnext::test::output::registry_source>()};
    const auto replacement_connection = fixture.registry.admit(replacement_source);
    REQUIRE(replacement_connection.has_value());
    CHECK(replacement_connection->target != fixture.connection.target);
    const auto replacement_context = fixture.owner.open(replacement_connection->target);
    REQUIRE(replacement_context.has_value());
    const auto stale_old_mutation = fixture.owner.submit(public_contexts[0U],
        inputnext::test::output::rumble_request(
            {.channels = rumble_mask, .sequence = 1U, .ttl = 1s}));
    CHECK((!stale_old_mutation.has_value() &&
           stale_old_mutation.error() == output_error::stale_context));
}

} // namespace
