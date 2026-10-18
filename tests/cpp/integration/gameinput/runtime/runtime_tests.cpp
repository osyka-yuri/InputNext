#include "gameinput/runtime/factory.hpp"

#include "gameinput/core/publication_source.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "runtime_metadata_test_support.hpp"
#include "runtime_test_support.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::gameinput_test_support;

TEST_CASE("gameinput.runtime.guide_merge_and_metadata_identity",
    "[gameinput][runtime][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto first = handle(fixture.device_memory[0U]);
    const auto second = handle(fixture.device_memory[1U]);
    fixture.connect(first);
    fixture.connect(second);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 2U);
    const auto first_connection = fixture.connection(0U);
    const auto second_connection = fixture.connection(1U);
    CHECK(first_connection.target != second_connection.target);

    const auto guide_mask = static_cast<button_mask>(gamepad_button::guide);
    fixture.native->publish(first,
        reading_parts{
            .guide = true,
            .source_timestamp_us = 100U,
            .observation_qpc = 1000U,
        });
    CHECK_FALSE(first_connection.reader.read().has_value());

    const auto native_reads_before = fixture.native->current_calls;
    fixture.native->publish(first, pad_parts(test_pad(0U, 0.5F), 101U, 1001U));
    const auto published = first_connection.reader.read();
    REQUIRE(published.has_value());
    CHECK((published->sample.state.buttons & guide_mask) != 0U);
    CHECK(published->sample.source_timestamp_us == 101U);

    for (std::size_t index = 0U; index < 20U; ++index) {
        CHECK(first_connection.reader.read().has_value());
    }
    CHECK(fixture.native->current_calls == native_reads_before);

    fixture.native->publish(first,
        reading_parts{
            .guide = false,
            .source_timestamp_us = 102U,
            .observation_qpc = 1002U,
        });
    const auto released = first_connection.reader.read();
    REQUIRE(released.has_value());
    CHECK((released->sample.state.buttons & guide_mask) == 0U);
}

TEST_CASE("gameinput.runtime.inline_callback_precedes_current_seed",
    "[gameinput][runtime][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(0U, 0.1F), 500U, 5000U));
    fixture.native->callback_on_next_reading_registration(
        pad_parts(test_pad(0U, 0.8F), 500U, 5001U));
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    const auto published = fixture.connection().reader.read();
    REQUIRE(published.has_value());
    CHECK(published->sample.state.left_stick.x == 0.8F);
    CHECK(published->sample.observation_qpc == 5001U);
}

TEST_CASE("gameinput.runtime.callback_failure_polls_and_retries",
    "[gameinput][runtime][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(0U, 0.4F), 200U, 2000U));
    fixture.native->fail_next_reading_registrations(1U);
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.connection().health == connection_health::polling_fallback);
    CHECK(fixture.connection().pad_known);
    const auto attempts = fixture.native->reading_registration_attempts;

    fixture.runtime->maintain(fixture.now + std::chrono::milliseconds{4});
    CHECK(fixture.native->reading_registration_attempts == attempts);
    const auto calls_before_retry = fixture.native->current_calls;
    fixture.runtime->maintain(fixture.now + std::chrono::seconds{1});
    CHECK(fixture.native->reading_registration_attempts == attempts + 1U);
    CHECK(fixture.runtime->connections().entries[0U].health == connection_health::callback_driven);
    CHECK(fixture.native->current_calls == calls_before_retry + 2U);
}

TEST_CASE("gameinput.runtime.next_deadline_ignores_polling_after_first_pad_seed",
    "[gameinput][runtime][deadline][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        pad_parts(test_pad(0U, 0.3F), 100U, 1000U));
    const auto first_maintenance = fixture.now;
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.connection().health == connection_health::callback_driven);
    CHECK(fixture.connection().pad_known);
    CHECK_FALSE(fixture.connection().guide_known);
    CHECK(fixture.native->current_calls == 2U);

    const auto later_maintenance = first_maintenance + std::chrono::milliseconds{8};
    fixture.runtime->maintain(later_maintenance);
    const auto deadline = fixture.runtime->next_deadline();
    CHECK(deadline.has_value());
    CHECK(deadline.value_or(later_maintenance) > later_maintenance);
    CHECK(fixture.native->current_calls == 2U);
}

TEST_CASE("gameinput.runtime.partial_watch_registration_token_is_drained",
    "[gameinput][runtime][failure][integration]") {
    const runtime_fixture fixture;
    fixture.native->fail_next_watch_registration_with_partial_token();
    fixture.native->fail_next_unregisters(1U);

    const auto started = fixture.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().code == gameinput_error::registration_failed);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);

    const auto pending = fixture.runtime->stop(fixture.now);
    CHECK_FALSE(pending.drained);
    CHECK(pending.pending_callback_owners == 1U);

    const auto still_pending = fixture.runtime->stop(fixture.now + std::chrono::seconds{1});
    CHECK_FALSE(still_pending.drained);
    CHECK(still_pending.pending_callback_owners == 1U);

    const auto drained = fixture.runtime->stop(fixture.now + std::chrono::seconds{2});
    CHECK(drained.drained);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          0U);
}

TEST_CASE("gameinput.runtime.same_handle_reconnect_creates_a_new_incarnation",
    "[gameinput][runtime][lifecycle][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    const auto old_target = fixture.connection().target;

    fixture.native->emit(device, native_device_event::disconnected);
    fixture.native->emit(device, native_device_event::connected);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.connection().target != old_target);
    CHECK(fixture.registry.live_count() == 1U);
    CHECK(fixture.native->retained_devices.load() == 2U);
    CHECK(fixture.native->released_devices.load() == 1U);
}

TEST_CASE("gameinput.runtime.partial_reading_token_holds_owner_until_unregister",
    "[gameinput][runtime][failure][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->fail_next_reading_registration_with_partial_token();
    fixture.native->fail_next_unregisters(1U);
    fixture.connect(device);
    fixture.maintain();

    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.native->has_active_reading(device));
    fixture.native->emit(device, native_device_event::disconnected);
    const auto retirement_time = fixture.now;
    fixture.maintain();
    CHECK(fixture.registry.live_count() == 0U);
    CHECK(fixture.registry.resident_count() == 1U);
    CHECK(fixture.native->released_devices.load() == 0U);
    const auto deadline = fixture.runtime->next_deadline();
    CHECK(deadline.has_value());
    CHECK(deadline.value_or(retirement_time) > retirement_time);

    fixture.runtime->maintain(fixture.now + std::chrono::seconds{1});
    CHECK(fixture.registry.resident_count() == 1U);
    CHECK(fixture.native->has_active_reading(device));

    fixture.runtime->maintain(fixture.now + std::chrono::seconds{2});
    CHECK(fixture.registry.resident_count() == 0U);
    CHECK_FALSE(fixture.native->has_active_reading(device));
    CHECK(fixture.native->released_devices.load() == 1U);
}

TEST_CASE("gameinput.runtime.full_owner_table_recovers_through_rescan",
    "[gameinput][runtime][capacity][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    constexpr std::size_t owner_capacity = 128U;
    for (std::size_t index = 0U; index < owner_capacity; ++index) {
        fixture.connect(handle(fixture.device_memory[index]));
    }
    fixture.connect(handle(fixture.device_memory[owner_capacity]));
    fixture.maintain();

    REQUIRE(fixture.connection_count() == gameinput_live_connection_capacity);
    CHECK(fixture.registry.live_count() == gameinput_live_connection_capacity);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 1U);

    fixture.native->emit(handle(fixture.device_memory[0U]), native_device_event::disconnected);
    fixture.maintain();
    CHECK(fixture.connection_count() == gameinput_live_connection_capacity);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 0U);

    fixture.maintain();
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 1U);

    fixture.native->emit(handle(fixture.device_memory[1U]), native_device_event::disconnected);
    fixture.maintain();
    CHECK(fixture.connection_count() == gameinput_live_connection_capacity);
    CHECK(fixture.registry.live_count() == gameinput_live_connection_capacity);
    CHECK(fixture.native->has_active_reading(handle(fixture.device_memory[owner_capacity])));
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
}

TEST_CASE("gameinput.runtime.failed_rescan_registration_retries_without_losing_watch",
    "[gameinput][runtime][capacity][failure][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    constexpr std::size_t owner_capacity = 128U;
    for (std::size_t index = 0U; index <= owner_capacity; ++index) {
        fixture.connect(handle(fixture.device_memory[index]));
    }
    fixture.native->fail_next_rescan_registrations(1U);
    const auto first_maintenance = fixture.now;
    fixture.maintain();

    CHECK(fixture.connection_count() == gameinput_live_connection_capacity);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 0U);
    CHECK(fixture.native->rescan_registration_attempts == 1U);

    fixture.runtime->maintain(first_maintenance + std::chrono::milliseconds{500});
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 0U);
    CHECK(fixture.native->rescan_registration_attempts == 1U);

    fixture.runtime->maintain(first_maintenance + std::chrono::seconds{1});
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::device_watch) ==
          1U);
    CHECK(fixture.native->active_registrations(fake_gameinput::registration_kind::rescan) == 1U);
    CHECK(fixture.native->rescan_registration_attempts == 2U);
}

TEST_CASE("gameinput.runtime.next_deadline_ignores_admission_retry_at_live_capacity",
    "[gameinput][runtime][capacity][deadline][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());

    std::array<connection_target, gameinput_live_connection_capacity - 1U> external_targets{};
    for (auto& target : external_targets) {
        std::unique_ptr<source_lifetime> candidate{std::make_unique<publication_source>()};
        auto admitted = fixture.registry.admit(candidate);
        REQUIRE(admitted.has_value());
        target = admitted->target;
    }

    fixture.connect(handle(fixture.device_memory[0U]));
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    for (std::size_t index = 1U; index <= gameinput_live_connection_capacity; ++index) {
        fixture.connect(handle(fixture.device_memory[index]));
    }

    const auto failed_admission_time = fixture.now;
    fixture.runtime->maintain(failed_admission_time);
    CHECK(fixture.connection_count() == 1U);
    for (const auto target : external_targets) {
        REQUIRE(fixture.registry.retire(target).has_value());
    }
    const auto reclaimed = fixture.registry.drain_retired();
    static_cast<void>(reclaimed);

    const auto full_capacity_time = failed_admission_time + std::chrono::milliseconds{4};
    fixture.runtime->maintain(full_capacity_time);
    REQUIRE(fixture.connection_count() == gameinput_live_connection_capacity);
    const auto deadline = fixture.runtime->next_deadline();
    CHECK(deadline.has_value());
    CHECK(deadline.value_or(full_capacity_time) > full_capacity_time);
}

TEST_CASE("gameinput.runtime.event_during_initial_facts_fetch_remains_pending",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->block_next_facts();
    fixture.connect(device);
    std::jthread maintenance;
    scoped_facts_release facts_release{*fixture.native};
    maintenance = std::jthread([&] { fixture.runtime->maintain(fixture.now); });
    const bool facts_entered = fixture.native->wait_for_blocked_facts();
    REQUIRE(facts_entered);

    fixture.native->update_facts(runtime_fixture::native_facts(u"initial-event-endpoint"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    facts_release.release();
    maintenance.join();

    const auto target = fixture.connection().target;
    const auto stale = fixture.runtime->query_metadata(target);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == metadata_error::stale);

    fixture.maintain();
    const auto refreshed = fixture.runtime->query_metadata(target);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->native.haptic_endpoint_id.value == u"initial-event-endpoint");
}

TEST_CASE("gameinput.runtime.retirement_waits_for_callback_drain",
    "[gameinput][runtime][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.registry.live_count() == 1U);
    REQUIRE(fixture.registry.resident_count() == 1U);

    fixture.native->fail_next_unregisters(1U);
    fixture.native->emit(device, native_device_event::disconnected);
    fixture.maintain();
    CHECK(fixture.registry.live_count() == 0U);
    CHECK(fixture.registry.resident_count() == 1U);
    CHECK(fixture.native->retained_devices.load() == 1U);
    CHECK(fixture.native->released_devices.load() == 0U);

    fixture.native->publish(device, pad_parts(test_pad(0U, 0.9F), 999U, 9999U));
    CHECK(fixture.connection_count() == 0U);
    fixture.runtime->maintain(fixture.now + std::chrono::seconds{1});
    CHECK(fixture.registry.resident_count() == 0U);
    CHECK(fixture.native->released_devices.load() == 1U);

    const auto old_target = fixture.connection().target;
    fixture.connect(device);
    fixture.runtime->maintain(fixture.now + std::chrono::seconds{1} + std::chrono::milliseconds{4});
    REQUIRE(fixture.connection_count() == 1U);
    CHECK(fixture.connection().target != old_target);
}

TEST_CASE("gameinput.runtime.metadata_event_invalidates_inflight_query",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    fixture.metadata->block_next_queries(1U);
    metadata_result<metadata_snapshot> old_query = std::unexpected(metadata_error::closed);
    std::jthread query;
    scoped_query_release query_release{*fixture.metadata, 1U};
    query = std::jthread([&] { old_query = fixture.runtime->query_metadata(target); });
    const bool query_entered = fixture.metadata->wait_for_blocked(1U);
    REQUIRE(query_entered);

    fixture.native->update_facts(runtime_fixture::native_facts(u"new-endpoint"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    query_release.release();
    query.join();

    REQUIRE_FALSE(old_query.has_value());
    CHECK(old_query.error() == metadata_error::stale);
    fixture.maintain();
    const auto refreshed = fixture.runtime->query_metadata(target);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->native.haptic_endpoint_id.value == u"new-endpoint");
}

TEST_CASE("gameinput.runtime.event_during_refresh_does_not_acknowledge_stale_facts",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    fixture.native->update_facts(runtime_fixture::native_facts(u"first-refresh"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    fixture.native->block_next_facts();
    std::jthread maintenance;
    scoped_facts_release facts_release{*fixture.native};
    maintenance = std::jthread([&] { fixture.runtime->maintain(fixture.now); });
    const bool facts_entered = fixture.native->wait_for_blocked_facts();
    REQUIRE(facts_entered);

    fixture.native->update_facts(runtime_fixture::native_facts(u"second-refresh"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    facts_release.release();
    maintenance.join();

    const auto pending = fixture.runtime->query_metadata(target);
    REQUIRE_FALSE(pending.has_value());
    CHECK(pending.error() == metadata_error::stale);

    fixture.runtime->maintain(fixture.now + std::chrono::milliseconds{4});
    const auto refreshed = fixture.runtime->query_metadata(target);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->native.haptic_endpoint_id.value == u"second-refresh");
}

TEST_CASE("gameinput.runtime.haptic_metadata_moves_from_not_present_to_available",
    "[gameinput][runtime][metadata][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    auto initial_facts = runtime_fixture::native_facts(u"");
    initial_facts.haptic_endpoint_id =
        metadata_field<std::u16string>{.availability = metadata_availability::not_present};
    fixture.native->update_facts(std::move(initial_facts));
    fixture.connect(device);
    fixture.maintain();

    const auto target = fixture.connection().target;
    const auto absent = fixture.runtime->query_metadata(target);
    REQUIRE(absent.has_value());
    CHECK(absent->native.haptic_endpoint_id.availability == metadata_availability::not_present);

    fixture.native->update_facts(runtime_fixture::native_facts(u"available-endpoint"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    fixture.maintain();
    const auto available = fixture.runtime->query_metadata(target);
    REQUIRE(available.has_value());
    CHECK(available->native.haptic_endpoint_id.availability == metadata_availability::available);
    CHECK(available->native.haptic_endpoint_id.value == u"available-endpoint");
}

TEST_CASE("gameinput.runtime.transient_facts_failure_preserves_metadata_and_retries_after_backoff",
    "[gameinput][runtime][metadata][failure][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    REQUIRE(fixture.runtime->query_metadata(target).has_value());

    fixture.native->update_facts(runtime_fixture::native_facts(u"retried-endpoint"));
    fixture.native->fail_next_facts(1U);
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    const auto initial_facts_calls = fixture.native->facts_calls;
    const auto first_refresh = fixture.now;
    fixture.maintain();
    CHECK(fixture.native->facts_calls == initial_facts_calls + 1U);

    const auto stale = fixture.runtime->query_metadata(target);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == metadata_error::stale);

    fixture.runtime->maintain(first_refresh + std::chrono::milliseconds{500});
    CHECK(fixture.native->facts_calls == initial_facts_calls + 1U);
    const auto still_stale = fixture.runtime->query_metadata(target);
    REQUIRE_FALSE(still_stale.has_value());
    CHECK(still_stale.error() == metadata_error::stale);

    fixture.runtime->maintain(first_refresh + std::chrono::seconds{1});
    CHECK(fixture.native->facts_calls == initial_facts_calls + 2U);
    const auto refreshed = fixture.runtime->query_metadata(target);
    REQUIRE(refreshed.has_value());
    CHECK(refreshed->native.haptic_endpoint_id.value == u"retried-endpoint");
}

TEST_CASE("gameinput.runtime.facts_generation_exhaustion_does_not_stop_input",
    "[gameinput][runtime][metadata][capacity][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto connection = fixture.connection();

    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(runtime_factory::seed_facts_generation(*fixture.runtime,
        connection.target,
        facts_generation_seed{.requested = maximum, .installed = maximum}));
    fixture.native->emit(device, native_device_event::haptic_info_updated);

    const auto exhausted = fixture.runtime->query_metadata(connection.target);
    REQUIRE_FALSE(exhausted.has_value());
    CHECK(exhausted.error() == metadata_error::resource_exhausted);

    fixture.native->publish(device, pad_parts(test_pad(0U, 0.7F), 800U, 8000U));
    const auto reading = connection.reader.read();
    REQUIRE(reading.has_value());
    CHECK(reading->sample.state.left_stick.x == 0.7F);
    CHECK(fixture.connection().health == connection_health::callback_driven);
}

TEST_CASE("gameinput.runtime.reconnect_during_facts_fetch_starts_new_incarnation",
    "[gameinput][runtime][lifecycle][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto old_target = fixture.connection().target;

    fixture.native->update_facts(runtime_fixture::native_facts(u"old-refresh"));
    fixture.native->emit(device, native_device_event::haptic_info_updated);
    fixture.native->block_next_facts();
    std::jthread maintenance;
    scoped_facts_release facts_release{*fixture.native};
    maintenance = std::jthread([&] { fixture.runtime->maintain(fixture.now); });
    REQUIRE(fixture.native->wait_for_blocked_facts());

    fixture.native->emit(device, native_device_event::disconnected);
    fixture.native->update_facts(runtime_fixture::native_facts(u"new-incarnation"));
    fixture.native->emit(device, native_device_event::connected);
    facts_release.release();
    maintenance.join();

    const auto old_metadata = fixture.runtime->query_metadata(old_target);
    REQUIRE_FALSE(old_metadata.has_value());
    CHECK(old_metadata.error() == metadata_error::not_connected);

    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    const auto current = fixture.connection();
    CHECK(current.target != old_target);
    fixture.maintain();
    const auto metadata = fixture.runtime->query_metadata(current.target);
    REQUIRE(metadata.has_value());
    CHECK(metadata->native.haptic_endpoint_id.value == u"new-incarnation");
}

TEST_CASE("gameinput.runtime.stop_waits_for_four_metadata_leases",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    constexpr std::size_t admitted_queries = 4U;
    fixture.metadata->block_next_queries(admitted_queries);
    std::array<metadata_result<metadata_snapshot>, admitted_queries> results{
        std::unexpected(metadata_error::closed),
        std::unexpected(metadata_error::closed),
        std::unexpected(metadata_error::closed),
        std::unexpected(metadata_error::closed)};
    std::array<std::jthread, admitted_queries> queries{};
    scoped_query_release query_release{*fixture.metadata, admitted_queries};
    for (std::size_t index = 0U; index < queries.size(); ++index) {
        queries[index] =
            std::jthread([&, index] { results[index] = fixture.runtime->query_metadata(target); });
    }

    const bool all_entered = fixture.metadata->wait_for_blocked(admitted_queries);
    REQUIRE(all_entered);

    const auto excess = fixture.runtime->query_metadata(target);
    REQUIRE_FALSE(excess.has_value());
    CHECK(excess.error() == metadata_error::busy);

    const auto stopped = fixture.runtime->stop(fixture.now);
    CHECK_FALSE(stopped.drained);
    CHECK(stopped.active_metadata_queries == admitted_queries);

    query_release.release();
    for (auto& thread : queries) {
        thread.join();
    }
    for (const auto& result : results) {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == metadata_error::stale);
    }

    const auto drained = fixture.runtime->stop(fixture.now + std::chrono::milliseconds{4});
    CHECK(drained.drained);
    CHECK(drained.active_metadata_queries == 0U);
}

TEST_CASE("gameinput.runtime.registry_replacement_invalidates_inflight_query",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    fixture.metadata->block_next_queries(1U);
    metadata_result<metadata_snapshot> old_query = std::unexpected(metadata_error::closed);
    std::jthread query;
    scoped_query_release query_release{*fixture.metadata, 1U};
    query = std::jthread([&] { old_query = fixture.runtime->query_metadata(target); });
    REQUIRE(fixture.metadata->wait_for_blocked(1U));

    auto replacement_candidate = std::make_unique<publication_source>();
    std::unique_ptr<source_lifetime> replacement_owner{std::move(replacement_candidate)};
    auto replacement = fixture.registry.replace(target, replacement_owner);
    REQUIRE(replacement.has_value());
    CHECK(replacement->target != target);

    query_release.release();
    query.join();
    REQUIRE_FALSE(old_query.has_value());
    CHECK(old_query.error() == metadata_error::stale);

    fixture.native->emit(device, native_device_event::disconnected);
    fixture.maintain();
    CHECK(fixture.registry.live_count() == 1U);
}

TEST_CASE("gameinput.runtime.inflight_reading_dispatch_keeps_retiring_owner_alive",
    "[gameinput][runtime][concurrency][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto first_device = handle(fixture.device_memory[0U]);
    const auto second_device = handle(fixture.device_memory[1U]);
    fixture.connect(first_device);
    fixture.connect(second_device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 2U);
    const auto first_reader = fixture.connection(0U).reader;
    const auto second_reader = fixture.connection(1U).reader;

    std::jthread callback;
    scoped_reading_dispatch_release reading_release{*fixture.native};
    callback = std::jthread([&] {
        fixture.native->publish_blocked(first_device, pad_parts(test_pad(0U, 0.6F), 700U, 7000U));
    });
    REQUIRE(fixture.native->wait_for_blocked_reading_dispatch());

    fixture.native->emit(first_device, native_device_event::disconnected);
    fixture.maintain();
    CHECK(fixture.connection_count() == 1U);
    CHECK(fixture.registry.live_count() == 1U);
    CHECK(fixture.registry.resident_count() == 2U);
    CHECK(fixture.native->released_devices.load() == 0U);

    fixture.native->publish(second_device, pad_parts(test_pad(0U, 0.9F), 701U, 7001U));
    const auto independent_read = second_reader.read();
    REQUIRE(independent_read.has_value());
    CHECK(independent_read->sample.state.left_stick.x == 0.9F);

    reading_release.release();
    callback.join();
    fixture.runtime->maintain(fixture.now + std::chrono::seconds{1});
    CHECK_FALSE(fixture.native->has_active_reading(first_device));
    CHECK(fixture.registry.live_count() == 1U);
    CHECK(fixture.registry.resident_count() == 1U);
    CHECK(fixture.native->released_devices.load() == 1U);
    CHECK_FALSE(first_reader.read().has_value());
}

} // namespace
