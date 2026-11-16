#include "connection_test_support.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

#include <optional>

#include <catch2/catch_test_macros.hpp>

namespace inputnext::gameinput_detail {
namespace {

using namespace test_support;

TEST_CASE("gameinput.output.observation_uses_only_the_exact_current_managed_backend",
    "[gameinput][output][integration][observation][replacement]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));

    const auto before_registration = fixture.connections->observe_backend(fixture.target);
    REQUIRE(before_registration.has_value());
    CHECK_FALSE(before_registration->has_value());

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    REQUIRE(wait_until([&fixture] {
        const auto observed = fixture.connections->observe_backend(fixture.target);
        if (!observed.has_value() || !observed.value().has_value()) {
            return false;
        }
        const auto& backend = checked_value(observed.value());
        return backend.execution.progress.last_completed_attempt.has_value() &&
               !backend.execution.progress.write_in_flight;
    }));
    const auto initial = fixture.connections->observe_backend(fixture.target);
    REQUIRE(initial.has_value());
    const auto initial_backend = checked_value(initial.value());
    CHECK(initial_backend.kind == output_backend_kind::gameinput_rumble);
    CHECK(initial_backend.execution.progress.backend ==
          checked_value(fixture.connections->backend_progress(fixture.target)).backend);
    CHECK(initial_backend.execution.authority_snapshot.target == fixture.target);
    CHECK(initial_backend.execution.projection_current);
    const auto initial_attempt =
        checked_value(initial_backend.execution.progress.last_completed_attempt);
    CHECK(initial_attempt.result.status == output::native_status::accepted);

    const auto retired_target = fixture.target;
    fixture.backing.native->set_connected(fixture.device, false);
    fixture.backing.native->emit(fixture.device, native_device_event::disconnected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 0U);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());

    REQUIRE(wait_until([&fixture, retired_target] {
        const auto observation = fixture.connections->observe_backend(retired_target);
        return observation.has_value() && !observation->has_value();
    }));
    const auto retired = fixture.connections->observe_backend(retired_target);
    REQUIRE(retired.has_value());
    CHECK_FALSE(retired.value().has_value());

    fixture.backing.native->set_connected(fixture.device, true);
    fixture.backing.native->emit(fixture.device, native_device_event::connected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 1U);
    fixture.target = fixture.backing.connection().target;
    REQUIRE(fixture.target != retired_target);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));

    const auto replacement = fixture.connections->observe_backend(fixture.target);
    REQUIRE(replacement.has_value());
    const auto replacement_backend = checked_value(replacement.value());
    CHECK(replacement_backend.kind == output_backend_kind::gameinput_rumble);
    CHECK(replacement_backend.execution.progress.backend.target == fixture.target);
    CHECK(replacement_backend.execution.progress.backend !=
          initial_backend.execution.progress.backend);
    const auto old_target_again = fixture.connections->observe_backend(retired_target);
    REQUIRE(old_target_again.has_value());
    CHECK_FALSE(old_target_again->has_value());
}

TEST_CASE("gameinput.output.rumble_support_tracks_the_exact_managed_backend",
    "[gameinput][output][integration][rumble][capabilities][replacement]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));

    CHECK_FALSE(fixture.connections->rumble_support(fixture.target).has_value());

    auto& native_capabilities = fixture.backing.native->device_facts.gameinput_capabilities.value;
    native_capabilities.low_frequency_rumble = true;
    native_capabilities.high_frequency_rumble = false;
    fixture.backing.native->emit(fixture.device, native_device_event::haptic_info_updated);
    fixture.backing.maintain();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));

    REQUIRE(wait_until([&fixture] {
        const auto capabilities = fixture.connections->rumble_support(fixture.target);
        return capabilities.has_value() && capabilities->low_frequency &&
               !capabilities->high_frequency && capabilities->backend.valid();
    }));
    const auto initial_support = fixture.connections->rumble_support(fixture.target);
    REQUIRE(initial_support.has_value());
    const auto initial_support_value = initial_support.value_or(managed_rumble_capabilities{});
    CHECK(initial_support_value.low_frequency);
    CHECK_FALSE(initial_support_value.high_frequency);
    const auto initial_backend = initial_support_value.backend;
    const auto current_progress = fixture.connections->backend_progress(fixture.target);
    REQUIRE(current_progress.has_value());
    const auto current_progress_value = current_progress.value_or(output::backend_progress{});
    CHECK(initial_backend == current_progress_value.backend);

    const auto retired_target = fixture.target;
    fixture.backing.native->set_connected(fixture.device, false);
    fixture.backing.native->emit(fixture.device, native_device_event::disconnected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 0U);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    CHECK_FALSE(fixture.connections->rumble_support(retired_target).has_value());

    fixture.backing.native->set_connected(fixture.device, true);
    fixture.backing.native->emit(fixture.device, native_device_event::connected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 1U);
    fixture.target = fixture.backing.connection().target;
    REQUIRE(fixture.target != retired_target);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    REQUIRE(wait_until([&fixture] {
        const auto capabilities = fixture.connections->rumble_support(fixture.target);
        return capabilities.has_value() && capabilities->low_frequency &&
               !capabilities->high_frequency && capabilities->backend.valid();
    }));
    const auto replacement_support = fixture.connections->rumble_support(fixture.target);
    REQUIRE(replacement_support.has_value());
    const auto replacement_support_value =
        replacement_support.value_or(managed_rumble_capabilities{});
    CHECK(replacement_support_value.low_frequency);
    CHECK_FALSE(replacement_support_value.high_frequency);
    CHECK(replacement_support_value.backend.target == fixture.target);
    CHECK(replacement_support_value.backend != initial_backend);
    const auto replacement_progress = fixture.connections->backend_progress(fixture.target);
    REQUIRE(replacement_progress.has_value());
    const auto replacement_progress_value =
        replacement_progress.value_or(output::backend_progress{});
    CHECK(replacement_support_value.backend == replacement_progress_value.backend);
}

TEST_CASE("gameinput.output.sony_bluetooth_never_reports_directinput_rumble_support",
    "[gameinput][output][integration][rumble][capabilities][bluetooth]") {
    connection_fixture fixture;
    configure_dualsense(fixture, metadata_device_model::dualsense, physical_transport::bluetooth);
    REQUIRE(fixture.start(false));

    const auto generic = checked_value(fixture.add_generic_connection());
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(generic));

    CHECK_FALSE(fixture.connections->backend_progress(fixture.target).has_value());
    CHECK_FALSE(fixture.connections->rumble_support(fixture.target).has_value());
    const auto generic_support = fixture.connections->rumble_support(generic);
    REQUIRE(generic_support.has_value());
    const auto generic_support_value = generic_support.value_or(managed_rumble_capabilities{});
    CHECK(generic_support_value.low_frequency);
    CHECK(generic_support_value.high_frequency);
    const auto generic_backend = fixture.connections->backend_progress(generic);
    REQUIRE(generic_backend.has_value());
    const auto generic_backend_value = generic_backend.value_or(output::backend_progress{});
    CHECK(generic_support_value.backend == generic_backend_value.backend);
}

TEST_CASE("gameinput.output.dualsense_usb_reports_rumble_after_live_qualification",
    "[gameinput][output][integration][rumble][capabilities][usb]") {
    connection_fixture fixture;
    configure_dualsense(fixture);
    REQUIRE(fixture.start(false));

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    const auto backend = fixture.connections->observe_backend(fixture.target);
    REQUIRE(backend.has_value());
    REQUIRE(backend->has_value());
    CHECK(backend->value_or(managed_backend_observation{}).kind ==
          output_backend_kind::dualsense_usb);

    REQUIRE(wait_until([&fixture] {
        const auto capabilities = fixture.connections->rumble_support(fixture.target);
        return capabilities.has_value() && capabilities->backend.valid() &&
               capabilities->low_frequency && capabilities->high_frequency;
    }));
    const auto capabilities = fixture.connections->rumble_support(fixture.target);
    REQUIRE(capabilities.has_value());
    const auto capabilities_value = capabilities.value_or(managed_rumble_capabilities{});
    CHECK(capabilities_value.backend ==
          checked_value(fixture.connections->backend_progress(fixture.target)).backend);
}

} // namespace
} // namespace inputnext::gameinput_detail
