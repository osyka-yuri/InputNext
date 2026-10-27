#include "connection_test_support.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>

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

} // namespace
} // namespace inputnext::gameinput_detail
