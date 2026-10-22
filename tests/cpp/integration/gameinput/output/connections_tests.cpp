#include "connection_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;
using namespace inputnext::gameinput_test_support;
using namespace test_support;

[[nodiscard]] bool wait_for_delivered_value(connection_fixture& fixture,
    const connection_target target,
    const std::optional<output::attempt_id> previous_attempt,
    const float expected_low,
    const float expected_high) {
    return wait_until([&fixture, target, previous_attempt, expected_low, expected_high] {
        const auto state = fixture.connections->backend_progress(target);
        if (!state.has_value()) {
            return false;
        }
        const auto& progress = state.value();
        if (progress.write_in_flight || !progress.last_completed_attempt.has_value()) {
            return false;
        }
        const auto& attempt = progress.last_completed_attempt.value();
        if (previous_attempt.has_value() && attempt.id == previous_attempt.value()) {
            return false;
        }
        return attempt.result.status == output::native_status::accepted &&
               attempt.output.channels == output::rumble_mask &&
               std::abs(attempt.output.values.rumble.low_frequency - expected_low) < 0.0001F &&
               std::abs(attempt.output.values.rumble.high_frequency - expected_high) < 0.0001F;
    });
}

TEST_CASE("gameinput.output.connection_owner_reuses_live_backend_after_admission_closes",
    "[gameinput][output][integration][admission][lifetime][xinput]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(true, true));
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.wait_for_backend(fixture.target));

    const auto assignment = checked_value(fixture.presentation->presentation().positions()[0U]);
    const auto before_vibration =
        checked_value(fixture.connections->backend_progress(fixture.target));
    const auto previous_vibration_attempt = before_vibration.last_completed_attempt.transform(
        [](const auto& attempt) { return attempt.id; });
    XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 65535U, .wRightMotorSpeed = 16384U};
    CHECK(fixture.presentation->presentation().set_state(xinput::flavor::v1_4,
              assignment.user_index,
              &vibration) == ERROR_SUCCESS);
    fixture.output_executor->request_drive();
    REQUIRE(wait_for_delivered_value(fixture,
        fixture.target,
        previous_vibration_attempt,
        1.0F,
        16384.0F / 65535.0F));
    const auto driven = checked_value(fixture.connections->backend_progress(fixture.target));
    REQUIRE(driven.backend.valid());
    const auto driven_attempt = checked_value(driven.last_completed_attempt);
    CHECK(driven_attempt.result.status == output::native_status::accepted);
    const auto delivered = checked_value(fixture.controls->last_value());
    CHECK(std::abs(delivered.low_frequency - 1.0F) < 0.0001F);
    CHECK(std::abs(delivered.high_frequency - (16384.0F / 65535.0F)) < 0.0001F);

    const auto previous_zero_attempt = driven_attempt.id;
    fixture.connections->close_admission();
    REQUIRE(wait_until([&fixture] { return fixture.connections->progress().admission_quiescent; }));
    const auto still_current = checked_value(fixture.connections->backend_progress(fixture.target));
    CHECK(still_current.phase == output::backend_phase::current);

    vibration = XINPUT_VIBRATION{};
    CHECK(fixture.presentation->presentation().set_state(xinput::flavor::v1_4,
              assignment.user_index,
              &vibration) == ERROR_SUCCESS);
    fixture.output_executor->request_drive();
    REQUIRE(wait_for_delivered_value(fixture, fixture.target, previous_zero_attempt, 0.0F, 0.0F));
    const auto zero = checked_value(fixture.controls->last_value());
    CHECK(zero.low_frequency == 0.0F);
    CHECK(zero.high_frequency == 0.0F);

    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.phase == connection_control_phase::stopped);
    CHECK(stopped.control_finished);
    CHECK(stopped.control_joined);
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
    CHECK(stopped.candidate_obligations == 0U);
    CHECK(stopped.backend_obligations == 0U);
    CHECK(stopped.pending_retirements == 0U);
}

TEST_CASE("gameinput.output.connection_owner_registers_before_first_pad_reading",
    "[gameinput][output][integration][connection][capability][readiness]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));
    const release_rumble_controls release{fixture.controls};

    const auto inventory = fixture.backing.runtime->connections();
    REQUIRE(inventory.size == 1U);
    CHECK(inventory.entries[0U].target == fixture.target);
    CHECK(inventory.entries[0U].health == connection_health::callback_driven);
    CHECK((!inventory.entries[0U].pad_known));

    const auto described_result = fixture.runtime_access->describe(fixture.target);
    REQUIRE(described_result.has_value());
    const auto& described = described_result.value();
    const auto capabilities = checked_value(described.capabilities);
    CHECK(capabilities.low_frequency);
    CHECK(capabilities.high_frequency);

    REQUIRE(fixture.connections->synchronize(inventory).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    CHECK(fixture.controls->factory_calls() == 1U);

    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
}

TEST_CASE("gameinput.output.connection_owner_recovers_unknown_target_and_autonomously_delivers",
    "[gameinput][output][integration][connection][recovery][autonomous]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(true, true));
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.wait_for_backend(fixture.target));

    const auto assignment = checked_value(fixture.presentation->presentation().positions()[0U]);
    auto unknown_target = fixture.target;
    ++unknown_target.device.value;
    ++unknown_target.connection.value;
    auto desired = fixture.backing.runtime->connections();
    REQUIRE(desired.size == 1U);
    desired.entries[1U] = desired.entries[0U];
    desired.entries[1U].target = unknown_target;
    desired.size = 2U;

    REQUIRE(fixture.connections->synchronize(desired).has_value());
    REQUIRE(wait_until([&fixture, unknown_target] {
        const auto report = fixture.connections->progress();
        if (!report.failure.has_value()) {
            return false;
        }
        const auto failure = checked_value(report.failure);
        return failure.operation == connection_control_operation::describe &&
               failure.target == unknown_target;
    }));

    const auto added_device = handle(fixture.backing.device_memory[1U]);
    fixture.backing.connect(added_device);
    fixture.backing.maintain();
    const auto inventory = fixture.backing.runtime->connections();
    REQUIRE(inventory.size == 2U);
    REQUIRE(inventory.entries[1U].target == unknown_target);
    REQUIRE(fixture.connections->synchronize(inventory).has_value());

    REQUIRE(fixture.wait_for_backend(unknown_target));
    REQUIRE(wait_for_delivered_value(fixture, unknown_target, std::nullopt, 0.0F, 0.0F));
    REQUIRE(wait_for_delivered_value(fixture, fixture.target, std::nullopt, 0.0F, 0.0F));
    REQUIRE(fixture.connections->progress().admission_quiescent);
    CHECK(fixture.controls->factory_calls() == 2U);
    const auto current_progress =
        checked_value(fixture.connections->backend_progress(fixture.target));
    const auto previous_attempt = current_progress.last_completed_attempt.transform(
        [](const auto& attempt) { return attempt.id; });

    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 24000U, .wRightMotorSpeed = 41000U};
    REQUIRE(fixture.presentation->presentation().set_state(xinput::flavor::v1_4,
                assignment.user_index,
                &vibration) == ERROR_SUCCESS);
    REQUIRE(wait_for_delivered_value(fixture,
        fixture.target,
        previous_attempt,
        24000.0F / 65535.0F,
        41000.0F / 65535.0F));

    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
}

TEST_CASE("gameinput.output.connection_owner_waits_for_same_native_instance_lease_return",
    "[gameinput][output][integration][connection][lifetime][reconnect]") {
    connection_fixture fixture;
    REQUIRE(fixture.start());
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.wait_for_backend(fixture.target));

    fixture.controls->block_next_endpoint_release();
    fixture.backing.native->set_connected(fixture.device, false);
    fixture.backing.native->emit(fixture.device, native_device_event::disconnected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 0U);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.controls->wait_for_endpoint_release());

    fixture.backing.native->set_connected(fixture.device, true);
    fixture.backing.native->emit(fixture.device, native_device_event::connected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 1U);
    const auto replacement_connection = fixture.backing.connection();
    const auto replacement = replacement_connection.target;
    REQUIRE(replacement != fixture.target);
    CHECK(replacement.device != fixture.target.device);
    CHECK(replacement.connection != fixture.target.connection);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());

    REQUIRE(wait_until([&fixture] {
        const auto report = fixture.connections->progress();
        if (!report.failure.has_value()) {
            return false;
        }
        const auto failure = checked_value(report.failure);
        if (failure.operation != connection_control_operation::reserve ||
            !failure.runtime_failure.has_value()) {
            return false;
        }
        const auto runtime_failure = checked_value(failure.runtime_failure);
        return runtime_failure.code == gameinput_error::resource_exhausted;
    }));
    CHECK(fixture.controls->factory_calls() == 1U);

    fixture.controls->release_endpoint_release();
    REQUIRE(fixture.wait_for_backend(replacement));
    CHECK(fixture.controls->factory_calls() == 2U);
    CHECK(fixture.controls->endpoint_releases() >= 1U);

    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
}

TEST_CASE("gameinput.output.connection_owner_cancels_native_instance_retry_on_stop",
    "[gameinput][output][integration][connection][shutdown][recovery]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));

    fixture.controls->block_next_endpoint_release();
    fixture.backing.native->set_connected(fixture.device, false);
    fixture.backing.native->emit(fixture.device, native_device_event::disconnected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 0U);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.controls->wait_for_endpoint_release());

    fixture.backing.native->set_connected(fixture.device, true);
    fixture.backing.native->emit(fixture.device, native_device_event::connected);
    fixture.backing.maintain();
    REQUIRE(fixture.backing.connection_count() == 1U);
    const auto replacement = fixture.backing.connection();
    CHECK(replacement.target.device != fixture.target.device);
    CHECK(replacement.target.connection != fixture.target.connection);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());

    REQUIRE(wait_until([&fixture] {
        const auto report = fixture.connections->progress();
        if (!report.failure.has_value()) {
            return false;
        }
        const auto failure = checked_value(report.failure);
        if (failure.operation != connection_control_operation::reserve ||
            !failure.runtime_failure.has_value()) {
            return false;
        }
        const auto runtime_failure = checked_value(failure.runtime_failure);
        return runtime_failure.code == gameinput_error::resource_exhausted;
    }));
    CHECK(fixture.controls->factory_calls() == 1U);

    fixture.connections->request_stop();
    fixture.controls->release_endpoint_release();
    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
    CHECK(fixture.controls->factory_calls() == 1U);
}

TEST_CASE("gameinput.output.connection_owner_keeps_all_resident_obligations_at_capacity",
    "[gameinput][output][integration][connection][capacity][lifetime]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));
    const release_rumble_controls release{fixture.controls};

    struct live_device final {
        std::size_t native_index{};
        connection_target target{};
    };

    std::array<live_device, gameinput_live_connection_capacity> active{};
    active[0U] = live_device{.native_index = 0U, .target = fixture.target};
    for (std::size_t index = 1U; index < active.size(); ++index) {
        const auto inventory_before = fixture.backing.runtime->connections();
        const auto next_device = handle(fixture.backing.device_memory[index]);
        fixture.backing.connect(next_device);
        fixture.backing.maintain();
        const auto inventory_after = fixture.backing.runtime->connections();
        REQUIRE(inventory_after.size == index + 1U);
        connection_target added{};
        for (const auto& connection :
            std::span{inventory_after.entries}.first(inventory_after.size)) {
            const auto found = std::ranges::find_if(
                std::span{inventory_before.entries}.first(inventory_before.size),
                [&](const auto& previous) { return previous.target == connection.target; });
            if (found == std::span{inventory_before.entries}.first(inventory_before.size).end()) {
                added = connection.target;
                break;
            }
        }
        REQUIRE(added.valid());
        active[index] = live_device{.native_index = index, .target = added};
    }

    auto inventory = fixture.backing.runtime->connections();
    REQUIRE(fixture.connections->synchronize(inventory).has_value());
    for (const auto& device : active) {
        REQUIRE(fixture.wait_for_backend(device.target));
    }
    CHECK(fixture.controls->factory_calls() == gameinput_live_connection_capacity);

    constexpr std::size_t retired_target_count = 112U;
    for (std::size_t cycle = 0U; cycle < retired_target_count; ++cycle) {
        const auto retiring = active[0U];
        fixture.controls->block_next_endpoint_release();
        const auto native = handle(fixture.backing.device_memory[retiring.native_index]);
        fixture.backing.native->set_connected(native, false);
        fixture.backing.native->emit(native, native_device_event::disconnected);
        fixture.backing.maintain();
        const auto without_retired = fixture.backing.runtime->connections();
        REQUIRE(without_retired.size == gameinput_live_connection_capacity - 1U);
        REQUIRE(fixture.connections->synchronize(without_retired).has_value());
        REQUIRE(fixture.controls->wait_for_endpoint_release_count(cycle + 1U));

        for (std::size_t index = 1U; index < active.size(); ++index) {
            active[index - 1U] = active[index];
        }
        const auto added_index = gameinput_live_connection_capacity + cycle;
        const auto added_native = handle(fixture.backing.device_memory[added_index]);
        fixture.backing.connect(added_native);
        fixture.backing.maintain();
        inventory = fixture.backing.runtime->connections();
        REQUIRE(inventory.size == gameinput_live_connection_capacity);

        connection_target added_target{};
        for (const auto& connection : std::span{inventory.entries}.first(inventory.size)) {
            const auto found =
                std::ranges::find_if(std::span{without_retired.entries}.first(without_retired.size),
                    [&](const auto& previous) { return previous.target == connection.target; });
            if (found == std::span{without_retired.entries}.first(without_retired.size).end()) {
                added_target = connection.target;
                break;
            }
        }
        REQUIRE(added_target.valid());
        active.back() = live_device{.native_index = added_index, .target = added_target};
        REQUIRE(fixture.connections->synchronize(inventory).has_value());
        REQUIRE(fixture.wait_for_backend(added_target));
    }

    REQUIRE(wait_until(
        [&fixture] {
            const auto report = fixture.connections->progress();
            return report.backend_obligations == 128U && report.pending_retirements == 112U;
        },
        10s));
    CHECK(fixture.controls->factory_calls() == 128U);
    const auto saturated = fixture.connections->progress();
    CHECK(saturated.candidate_obligations == 0U);
    CHECK(saturated.backend_obligations == 128U);
    CHECK(saturated.pending_retirements == 112U);
    for (const auto& device : active) {
        const auto state = checked_value(fixture.connections->backend_progress(device.target));
        CHECK(state.phase == output::backend_phase::current);
        CHECK(state.backend.valid());
    }

    fixture.controls->release_endpoint_releases(retired_target_count);
    REQUIRE(wait_until(
        [&fixture] { return fixture.controls->endpoint_releases() == retired_target_count; },
        10s));
    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.backend_obligations == 0U);
    CHECK(stopped.pending_retirements == 0U);
    CHECK(stopped.executor_shutdown_returned);
}

TEST_CASE("gameinput.output.connection_owner_retries_factory_without_new_inventory",
    "[gameinput][output][integration][connection][retry][recovery]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));
    const release_rumble_controls release{fixture.controls};
    fixture.controls->fail_next_factory();

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    CHECK(fixture.controls->factory_calls() == 2U);
    CHECK(fixture.connections->progress().failure.has_value());

    fixture.presentation->close();
    fixture.connections->shutdown();
    CHECK(fixture.connections->progress().all_retirements_acknowledged);
}

TEST_CASE("gameinput.output.connection_owner_cancels_candidate_when_admission_closes",
    "[gameinput][output][integration][connection][admission][concurrency]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(false));
    const release_rumble_controls release{fixture.controls};
    fixture.controls->block_next_factory();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.controls->wait_for_factory());
    CHECK((!fixture.connections->progress().admission_quiescent));
    CHECK(fixture.connections->progress().candidate_obligations == 1U);

    fixture.connections->close_admission();
    fixture.controls->release_factory();
    REQUIRE(wait_until([&fixture] { return fixture.connections->progress().admission_quiescent; }));
    CHECK(fixture.connections->progress().backend_obligations == 0U);
    CHECK(fixture.controls->factory_calls() == 1U);
    CHECK(fixture.controls->endpoint_releases() == 1U);

    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
}

TEST_CASE("gameinput.output.connection_owner_rechecks_pending_stop_after_last_inventory",
    "[gameinput][output][integration][connection][shutdown][concurrency]") {
    connection_fixture fixture;
    REQUIRE(fixture.start(true, true));
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.wait_for_backend(fixture.target));
    const auto assignment = checked_value(fixture.presentation->presentation().positions()[0U]);

    REQUIRE(wait_until([&fixture] {
        const auto state = fixture.connections->backend_progress(fixture.target);
        const auto value = fixture.controls->last_value();
        if (fixture.controls->writes() == 0U || !state.has_value() || !value.has_value()) {
            return false;
        }
        const auto& progress = state.value();
        const auto& delivered = value.value();
        return progress.last_completed_attempt.has_value() && !progress.write_in_flight &&
               delivered.low_frequency == 0.0F && delivered.high_frequency == 0.0F;
    }));
    const auto before_mutation =
        checked_value(fixture.connections->backend_progress(fixture.target));
    const auto completed_before_mutation = checked_value(before_mutation.last_completed_attempt);
    const auto writes_before_mutation = fixture.controls->writes();
    const auto previous_mutation_attempt = completed_before_mutation.id;

    fixture.controls->block_next_write();
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 30000U, .wRightMotorSpeed = 40000U};
    REQUIRE(fixture.presentation->presentation().set_state(xinput::flavor::v1_4,
                assignment.user_index,
                &vibration) == ERROR_SUCCESS);
    fixture.output_executor->request_drive();
    REQUIRE(fixture.controls->wait_for_write());
    REQUIRE(wait_until([&fixture, previous_mutation_attempt] {
        const auto state = fixture.connections->backend_progress(fixture.target);
        if (!state.has_value()) {
            return false;
        }
        const auto& progress = state.value();
        return progress.write_in_flight && progress.last_attempt.has_value() &&
               progress.last_attempt.value().value != previous_mutation_attempt.value;
    }));
    CHECK(fixture.controls->writes() == writes_before_mutation);

    fixture.connections->request_stop();
    REQUIRE(wait_until(
        [&fixture] { return fixture.connections->progress().pending_retirements == 1U; }));
    const auto pending = fixture.connections->progress();
    CHECK((!pending.control_finished));
    CHECK(pending.backend_obligations == 1U);
    CHECK(pending.pending_retirements == 1U);

    fixture.controls->release_write();
    fixture.presentation->close();
    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.control_finished);
    CHECK(stopped.control_joined);
    CHECK(stopped.all_retirements_acknowledged);
    CHECK(stopped.executor_shutdown_returned);
    CHECK(stopped.backend_obligations == 0U);
    CHECK(stopped.pending_retirements == 0U);
}

TEST_CASE("gameinput.output.connection_owner_preserves_unresolved_backend_until_executor_shutdown",
    "[gameinput][output][integration][connection][shutdown][recovery]") {
    connection_fixture fixture;
    REQUIRE(fixture.start());
    const release_rumble_controls release{fixture.controls};
    REQUIRE(fixture.wait_for_backend(fixture.target));

    fixture.presentation->close();
    fixture.output_executor->shutdown();
    REQUIRE(wait_until([&fixture] {
        const auto state = fixture.connections->progress();
        return state.phase == connection_control_phase::needs_executor_shutdown &&
               state.failure.has_value();
    }));
    const auto unresolved = fixture.connections->progress();
    CHECK((!unresolved.all_retirements_acknowledged));
    CHECK(unresolved.backend_obligations == 1U);

    fixture.connections->shutdown();
    const auto stopped = fixture.connections->progress();
    CHECK(stopped.phase == connection_control_phase::stopped);
    CHECK((!stopped.all_retirements_acknowledged));
    CHECK(stopped.executor_shutdown_returned);
    CHECK(stopped.backend_obligations == 0U);
    CHECK(stopped.failure.has_value());
}

} // namespace
} // namespace inputnext::gameinput_detail
