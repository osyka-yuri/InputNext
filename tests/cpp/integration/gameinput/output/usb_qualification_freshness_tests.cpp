#include "connection_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/usb/qualification.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;
using namespace test_support;

[[nodiscard]] metadata_field<metadata_device_model> dualsense_model() noexcept {
    return {.availability = metadata_availability::available,
        .value = metadata_device_model::dualsense};
}

[[nodiscard]] metadata_field<transport_metadata> transport_field(
    const metadata_availability availability,
    const physical_transport transport = physical_transport::usb) noexcept {
    return {.availability = availability,
        .value = {.value = transport, .provenance = transport_provenance::native_bus_type}};
}

[[nodiscard]] bool backend_is_retired(const connection_fixture& fixture) {
    const auto report = fixture.connections->progress();
    return !fixture.connections->backend_progress(fixture.target).has_value() &&
           report.backend_obligations == 0U && report.candidate_obligations == 0U;
}

void set_observation(connection_fixture const& fixture,
    const metadata_availability transport_availability,
    const physical_transport transport = physical_transport::usb,
    const bool conflicting_identity = false) {
    fixture.backing.metadata->set_observation(dualsense_model(),
        transport_field(transport_availability, transport),
        conflicting_identity);
}

[[nodiscard]] output::backend_handle current_backend(const connection_fixture& fixture) {
    const auto progress = fixture.connections->backend_progress(fixture.target);
    if (!progress.has_value() || progress->phase != output::backend_phase::current) {
        throw std::runtime_error("Expected a current USB backend.");
    }
    return progress->backend;
}

[[nodiscard]] bool wait_for_current_usb_route(const connection_fixture& fixture) {
    return wait_until([&fixture] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::current &&
               progress->represented_channels == output::supported_channel_mask;
    });
}

[[nodiscard]] output::result<output::public_context> open_usb_context(
    connection_fixture const& fixture) {
    return fixture.composition->public_authority().open(fixture.target);
}

TEST_CASE(
    "gameinput.output.usb_qualification_recovers_from_unknown_transport_without_catalogue_change",
    "[gameinput][output][integration][dualsense][qualification][recovery]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    set_observation(fixture, metadata_availability::unavailable);
    REQUIRE(fixture.start(false, true));
    const auto completed_before_sync = fixture.backing.metadata->completed_queries();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_until([&fixture, completed_before_sync] {
        return fixture.backing.metadata->completed_queries() > completed_before_sync;
    }));
    CHECK((!fixture.connections->backend_progress(fixture.target).has_value()));
    CHECK(fixture.raw_controls->factory_calls() == 0U);

    const auto before = fixture.runtime_access->describe(fixture.target).value();
    set_observation(fixture, metadata_availability::available);
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 1U; }));
    const auto after = fixture.runtime_access->describe(fixture.target).value();
    CHECK(after.catalogue_generation == before.catalogue_generation);
    CHECK(fixture.raw_controls->factory_calls() == 1U);
}

TEST_CASE("gameinput.output.usb_qualification_revokes_routes_when_transport_or_identity_changes",
    "[gameinput][output][integration][dualsense][qualification][revocation]") {
    for (const auto invalidation : {0U, 1U}) {
        connection_fixture fixture;
        configure_dualsense(fixture);
        REQUIRE(fixture.start(false));
        REQUIRE(
            fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
        REQUIRE(wait_for_current_usb_route(fixture));
        REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 1U; }));
        const auto catalogue_generation =
            fixture.runtime_access->describe(fixture.target).value().catalogue_generation;

        const auto sends_before = fixture.raw_controls->send_calls();
        if (invalidation == 0U) {
            set_observation(fixture,
                metadata_availability::available,
                physical_transport::bluetooth);
        } else {
            set_observation(fixture,
                metadata_availability::available,
                physical_transport::usb,
                true);
        }

        REQUIRE(wait_until([&fixture] { return backend_is_retired(fixture); }));
        CHECK(fixture.runtime_access->describe(fixture.target).value().catalogue_generation ==
              catalogue_generation);
        const auto sends_after_retirement = fixture.raw_controls->send_calls();
        const auto completed_after_retirement = fixture.backing.metadata->completed_queries();
        REQUIRE(wait_until([&fixture, completed_after_retirement] {
            return fixture.backing.metadata->completed_queries() > completed_after_retirement;
        }));
        CHECK(fixture.raw_controls->send_calls() == sends_after_retirement);
        CHECK(sends_after_retirement >= sends_before);
        CHECK(fixture.raw_controls->factory_calls() == 1U);
    }
}

TEST_CASE("gameinput.output.usb_qualification_error_recovery_uses_a_new_backend_incarnation",
    "[gameinput][output][integration][dualsense][qualification][recovery]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 1U; }));
    const auto previous = current_backend(fixture);

    fixture.backing.metadata->fail_next_query(metadata_error::busy);
    REQUIRE(wait_until([&fixture] {
        const auto failure = fixture.connections->progress().failure;
        return failure.has_value() && failure->metadata_failure == metadata_error::busy;
    }));
    REQUIRE(wait_until([&fixture, previous] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::current &&
               progress->backend != previous;
    }));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 2U; }));
    CHECK(fixture.raw_controls->factory_calls() == 2U);
}

TEST_CASE("gameinput.output.usb_qualification_retries_creation_after_a_slow_initial_query",
    "[gameinput][output][integration][dualsense][qualification][recovery]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));
    fixture.backing.metadata->block_next_queries(1U);
    scoped_query_release release{*fixture.backing.metadata, 1U};
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));

    std::this_thread::sleep_for(usb_output_qualification::validity + 8ms);
    CHECK(fixture.raw_controls->factory_calls() == 0U);
    release.release();

    REQUIRE(wait_until([&fixture] {
        const auto failure = fixture.connections->progress().failure;
        return failure.has_value() && failure->metadata_failure == metadata_error::stale;
    }));
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&fixture] { return fixture.raw_controls->factory_calls() == 1U; }));
}

TEST_CASE("gameinput.output.usb_qualification_recovers_after_rescan_facts_refresh",
    "[gameinput][output][integration][dualsense][qualification][recovery]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));

    const auto unknown_device = handle(fixture.backing.device_memory[1U]);
    fixture.backing.native->emit(unknown_device, native_device_event::disconnected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.native->rescan_registration_attempts == 1U);
    const auto pending = fixture.backing.runtime->query_metadata(fixture.target);
    REQUIRE_FALSE(pending.has_value());
    REQUIRE(pending.error() == metadata_error::stale);

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_until([&fixture] {
        const auto failure = fixture.connections->progress().failure;
        return failure.has_value() && failure->metadata_failure == metadata_error::stale;
    }));
    CHECK(fixture.raw_controls->factory_calls() == 0U);

    fixture.backing.maintain();
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&fixture] { return fixture.raw_controls->factory_calls() == 1U; }));
    CHECK(fixture.backing.connection().target == fixture.target);
}

TEST_CASE("gameinput.output.usb_qualification_expiry_blocks_native_writes_and_not_input_reads",
    "[gameinput][output][integration][dualsense][qualification][expiry][concurrency]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false, true));
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 1U; }));
    const auto previous = current_backend(fixture);

    fixture.backing.metadata->block_next_queries(1U);
    scoped_query_release release{*fixture.backing.metadata, 1U};
    REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));
    std::this_thread::sleep_for(usb_output_qualification::validity + 8ms);

    const auto positions = fixture.presentation->presentation().positions();
    REQUIRE(positions[0U].has_value());
    XINPUT_STATE state{};
    CHECK(fixture.presentation->presentation().get_state(xinput::flavor::v1_4,
              checked_value(positions[0U]).user_index,
              &state) == ERROR_SUCCESS);

    REQUIRE(wait_until([&fixture] {
        const auto progress = fixture.connections->query_backend_progress(fixture.target);
        return progress.has_value() && !progress->write_in_flight;
    }));
    const auto before_denied_write =
        checked_value(fixture.connections->query_backend_progress(fixture.target));
    const auto previous_attempt = before_denied_write.last_attempt;
    const auto sends_before_denied_write = fixture.raw_controls->send_calls();
    auto context = open_usb_context(fixture);
    REQUIRE(context.has_value());
    const output::channel_values values{.rumble = {.low_frequency = 0.5F, .high_frequency = 0.25F}};
    REQUIRE(fixture.composition->public_authority()
            .submit(*context,
                {.selected_channels = output::supported_channel_mask,
                    .sequence = 1U,
                    .ttl = 2s,
                    .values = values})
            .has_value());
    fixture.output_executor->request_drive();
    REQUIRE(wait_until([&fixture, previous_attempt, values] {
        const auto progress = fixture.connections->query_backend_progress(fixture.target);
        return progress.has_value() && progress->last_completed_attempt.has_value() &&
               (!previous_attempt.has_value() ||
                   progress->last_completed_attempt->id != *previous_attempt) &&
               progress->last_completed_attempt->result.status ==
                   output::native_status::retryable_failure &&
               progress->last_completed_attempt->output.values == values;
    }));
    CHECK(fixture.raw_controls->send_calls() == sends_before_denied_write);

    release.release();
    REQUIRE(wait_until([&fixture, previous] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::current &&
               progress->backend != previous;
    }));
}

TEST_CASE("gameinput.output.usb_qualification_waits_for_old_native_work_before_replacement",
    "[gameinput][output][integration][dualsense][qualification][retirement][concurrency]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_for_current_usb_route(fixture));
    const auto previous = current_backend(fixture);
    fixture.raw_controls->block_next_send();
    fixture.raw_controls->block_next_report_release();

    auto context = open_usb_context(fixture);
    REQUIRE(context.has_value());
    const output::channel_values values{.rumble = {.low_frequency = 0.75F, .high_frequency = 0.5F}};
    REQUIRE(fixture.composition->public_authority()
            .submit(*context,
                {.selected_channels = output::supported_channel_mask,
                    .sequence = 1U,
                    .ttl = 2s,
                    .values = values})
            .has_value());
    fixture.output_executor->request_drive();
    REQUIRE(fixture.raw_controls->wait_for_send());

    set_observation(fixture, metadata_availability::available, physical_transport::bluetooth);
    REQUIRE(wait_until(
        [&fixture] { return fixture.connections->progress().pending_retirements == 1U; }));
    set_observation(fixture, metadata_availability::available);
    CHECK(fixture.raw_controls->factory_calls() == 1U);
    const auto retiring = checked_value(fixture.connections->backend_progress(fixture.target));
    CHECK(retiring.backend == previous);

    fixture.raw_controls->release_send();
    REQUIRE(fixture.raw_controls->wait_for_report_release());
    CHECK(fixture.raw_controls->factory_calls() == 1U);
    fixture.raw_controls->release_report_release();
    REQUIRE(wait_until([&fixture, previous] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::current &&
               progress->backend != previous;
    }));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 2U; }));
    CHECK(fixture.raw_controls->factory_calls() == 2U);
}

TEST_CASE("gameinput.output.usb_qualification_renews_existing_route_after_admission_closes",
    "[gameinput][output][integration][dualsense][qualification][admission]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_for_current_usb_route(fixture));
    REQUIRE(wait_until([&] { return fixture.raw_controls->factory_calls() == 1U; }));
    const auto backend = current_backend(fixture);
    const auto completed_before_close = fixture.backing.metadata->completed_queries();

    fixture.connections->close_admission();
    REQUIRE(wait_until(
        [&fixture, completed_before_close] {
            return fixture.backing.metadata->completed_queries() >= completed_before_close + 5U;
        },
        2s));
    const auto still_current = checked_value(fixture.connections->backend_progress(fixture.target));
    CHECK(still_current.phase == output::backend_phase::current);
    CHECK(still_current.backend == backend);
    CHECK(fixture.raw_controls->factory_calls() == 1U);
    const auto closed_synchronize = fixture.connections->synchronize(connection_inventory{});
    REQUIRE_FALSE(closed_synchronize.has_value());
    CHECK(closed_synchronize.error().code == gameinput_error::closed);
}

TEST_CASE("gameinput.output.usb_qualification_discards_results_for_removed_or_stopped_targets",
    "[gameinput][output][integration][dualsense][qualification][stale]") {
    for (const auto stop : {false, true}) {
        connection_fixture fixture;
        configure_dualsense(fixture);
        REQUIRE(fixture.start(false));
        fixture.backing.metadata->block_next_queries(1U);
        scoped_query_release release{*fixture.backing.metadata, 1U};
        REQUIRE(
            fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
        REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));

        if (stop) {
            fixture.connections->request_stop();
        } else {
            REQUIRE(fixture.connections->synchronize(connection_inventory{}).has_value());
        }
        release.release();

        if (stop) {
            REQUIRE(wait_until(
                [&fixture] { return fixture.connections->progress().control_finished; }));
            fixture.connections->shutdown();
        } else {
            REQUIRE(wait_until(
                [&fixture] { return fixture.connections->progress().admission_quiescent; }));
        }
        CHECK(fixture.raw_controls->factory_calls() == 0U);
        CHECK((!fixture.connections->backend_progress(fixture.target).has_value()));
    }
}

} // namespace
} // namespace inputnext::gameinput_detail
