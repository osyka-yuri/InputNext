#include "gameinput/runtime/output_access.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/rumble/rumble_sink.hpp"
#include "runtime_rumble_test_support.hpp"
#include "runtime_test_support.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::gameinput_test_support;

[[nodiscard]] bool wait_for_runtime_drain(gameinput_runtime& runtime,
    maintenance_time now,
    gameinput_stop_report& report) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        report = runtime.stop(now);
        if (report.drained) {
            return true;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return report.drained;
}

TEST_CASE("gameinput.runtime.output_access_owns_exact_lease_and_rumble_sink",
    "[gameinput][runtime][rumble][integration]") {
    runtime_fixture fixture;
    fixture.native->device_facts.gameinput_capabilities =
        metadata_field<native_gameinput_capabilities>{
            .availability = metadata_availability::available,
            .value = {.low_frequency_rumble = true, .high_frequency_rumble = false},
    };
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    CHECK((!runtime_output_access::create(*fixture.runtime).has_value()));
    REQUIRE(fixture.start().has_value());
    CHECK((!runtime_output_access::create(*fixture.runtime).has_value()));

    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    const auto target = fixture.connection().target;
    const auto description = access->describe(target);
    REQUIRE(description.has_value());
    CHECK(description->target == target);
    REQUIRE(description->capabilities.has_value());
    const auto capabilities = description->capabilities.value_or(rumble_capabilities{});
    CHECK(capabilities.low_frequency);
    CHECK((!capabilities.high_frequency));

    auto lease_result = access->reserve(target);
    REQUIRE(lease_result.has_value());
    const auto duplicate = access->reserve(target);
    CHECK((!duplicate.has_value()));
    auto lease = std::move(*lease_result);
    CHECK(lease.target() == target);

    auto sink_result = make_rumble_sink(std::move(lease));
    REQUIRE(sink_result.has_value());
    auto sink = std::move(*sink_result);
    output::native_command command{
        .backend = output::backend_handle{.target = target},
        .output =
            output::native_output{
                .channels = output::rumble_mask,
                .values =
                    output::channel_values{
                        .rumble =
                            output::rumble_value{.low_frequency = 0.25F, .high_frequency = 0.75F},
                    },
            },
    };
    CHECK(sink->write(command).status == output::native_status::accepted);
    CHECK(fixture.native->rumble_script()->writes() == 1U);
    CHECK(fixture.native->rumble_script()->last_value() == command.output.values.rumble);

    auto wrong_target = command;
    wrong_target.backend.target.connection.value += 1U;
    CHECK(sink->write(wrong_target).status == output::native_status::disconnected);
    auto wrong_mask = command;
    wrong_mask.output.channels = output::rumble_mask | output::leds_mask;
    CHECK(sink->write(wrong_mask).status == output::native_status::unsupported);
    CHECK(fixture.native->rumble_script()->writes() == 1U);

    sink.reset();
    CHECK(fixture.native->rumble_script()->endpoint_releases() == 1U);
    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
    CHECK(stopped.pending_output_leases == 0U);
    CHECK(stopped.native_api_cleanup_complete);
}

TEST_CASE("gameinput.runtime.output_sink_factory_failure_releases_candidate_lease",
    "[gameinput][runtime][rumble][rollback][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    const auto controls = fixture.native->rumble_script();
    controls->fail_next_factory();
    auto failed_lease = access->reserve(target);
    REQUIRE(failed_lease.has_value());
    CHECK((!make_rumble_sink(std::move(*failed_lease)).has_value()));

    auto retry_lease = access->reserve(target);
    REQUIRE(retry_lease.has_value());
    auto sink = make_rumble_sink(std::move(*retry_lease));
    REQUIRE(sink.has_value());
    CHECK(controls->factory_calls() == 2U);
    sink->reset();

    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
}

TEST_CASE("gameinput.runtime.without_output_access_keeps_caller_driven_native_release",
    "[gameinput][runtime][rumble][compatibility][integration]") {
    runtime_fixture fixture;
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);

    const auto controls = fixture.native->rumble_script();
    controls->block_next_device_release();
    std::jthread maintenance([&] {
        fixture.native->emit(device, native_device_event::disconnected);
        fixture.runtime->maintain(fixture.now + 4ms);
    });
    const scoped_rumble_release release_guard{*controls};
    CHECK(controls->wait_for_device_release());
    CHECK(controls->device_releases() == 0U);
    controls->release_device_release();
    maintenance.join();
    CHECK(controls->device_releases() == 1U);

    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now + 8ms, stopped));
    CHECK(stopped.native_api_cleanup_complete);
    fixture.runtime.reset();
    CHECK(controls->native_cleanup_complete());
}

TEST_CASE("gameinput.runtime.output_cleanup_keeps_input_progress_and_reports_blocked_release",
    "[gameinput][runtime][rumble][concurrency][integration]") {
    runtime_fixture fixture;
    const auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);

    const auto controls = fixture.native->rumble_script();
    const scoped_rumble_release release_guard{*controls};
    controls->block_next_device_release();
    controls->block_native_cleanup();
    fixture.native->emit(device, native_device_event::disconnected);
    fixture.runtime->maintain(fixture.now + 4ms);
    CHECK(controls->wait_for_device_release());

    // Native Release is on the cleanup worker; caller-driven maintenance still returns.
    fixture.runtime->maintain(fixture.now + 8ms);
    const auto pending_release = fixture.runtime->stop(fixture.now + 12ms);
    CHECK((!pending_release.drained));
    CHECK(pending_release.pending_native_cleanup_owners == 1U);
    CHECK((!pending_release.native_api_cleanup_complete));
    CHECK(controls->device_releases() == 0U);

    controls->release_device_release();
    const auto release_deadline = std::chrono::steady_clock::now() + 2s;
    while (
        controls->device_releases() == 0U && std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::yield();
    }
    CHECK(controls->device_releases() == 1U);
    const auto cleanup_deadline = std::chrono::steady_clock::now() + 2s;
    auto wait_api_cleanup = fixture.runtime->stop(fixture.now + 16ms);
    while (wait_api_cleanup.pending_native_cleanup_owners != 0U &&
           std::chrono::steady_clock::now() < cleanup_deadline) {
        std::this_thread::yield();
        wait_api_cleanup = fixture.runtime->stop(fixture.now + 16ms);
    }
    CHECK((!wait_api_cleanup.drained));
    CHECK(controls->wait_for_native_cleanup());
    const auto pending_api_cleanup = fixture.runtime->stop(fixture.now + 20ms);
    CHECK((!pending_api_cleanup.drained));
    CHECK(pending_api_cleanup.pending_native_cleanup_owners == 0U);
    CHECK((!pending_api_cleanup.native_api_cleanup_complete));

    controls->release_native_cleanup();
    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now + 24ms, drained));
    CHECK(drained.native_api_cleanup_complete);
    CHECK(controls->device_releases() == 1U);
    CHECK(controls->native_cleanup_complete());
}

TEST_CASE("gameinput.runtime.output_setter_keeps_device_api_and_lease_alive",
    "[gameinput][runtime][rumble][concurrency][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    auto lease_result = access->reserve(target);
    REQUIRE(lease_result.has_value());
    auto lease = std::move(*lease_result);
    auto endpoint_result = lease.create_rumble_endpoint();
    REQUIRE(endpoint_result.has_value());
    auto endpoint = std::move(*endpoint_result);

    const auto controls = fixture.native->rumble_script();
    controls->block_next_write();
    std::jthread setter([&] {
        static_cast<void>(endpoint->set(output::rumble_value{
            .low_frequency = 0.5F,
            .high_frequency = 0.75F,
        }));
    });
    const scoped_rumble_release release_guard{*controls};
    CHECK(controls->wait_for_write());

    const auto pending = fixture.runtime->stop(fixture.now);
    CHECK((!pending.drained));
    CHECK(pending.pending_output_leases == 1U);
    CHECK(pending.pending_native_cleanup_owners == 0U);
    CHECK((!pending.native_api_cleanup_complete));
    CHECK(controls->device_releases() == 0U);

    controls->release_write();
    setter.join();
    endpoint.reset();
    lease = runtime_output_lease{};
    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now + 4ms, drained));
    CHECK(drained.pending_output_leases == 0U);
    CHECK(controls->writes() == 1U);
    CHECK(controls->device_releases() == 1U);
}

TEST_CASE("gameinput.runtime.output_lease_and_endpoint_release_keep_native_owner_resident",
    "[gameinput][runtime][rumble][concurrency][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    auto lease_result = access->reserve(target);
    REQUIRE(lease_result.has_value());
    auto lease = std::move(*lease_result);
    auto endpoint_result = lease.create_rumble_endpoint();
    REQUIRE(endpoint_result.has_value());
    auto endpoint = std::move(*endpoint_result);

    const auto controls = fixture.native->rumble_script();
    controls->block_next_endpoint_release();
    std::jthread endpoint_cleanup(
        [](std::unique_ptr<native_rumble_endpoint> endpoint) { endpoint.reset(); },
        std::move(endpoint));
    const scoped_rumble_release release_guard{*controls};
    CHECK(controls->wait_for_endpoint_release());

    const auto blocked = fixture.runtime->stop(fixture.now);
    CHECK((!blocked.drained));
    CHECK(blocked.pending_output_leases == 1U);
    CHECK(blocked.pending_native_cleanup_owners == 0U);
    CHECK((!blocked.native_api_cleanup_complete));
    CHECK(fixture.native->released_devices.load() == 0U);

    controls->release_endpoint_release();
    endpoint_cleanup.join();
    lease = runtime_output_lease{};
    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now + 4ms, drained));
    CHECK(drained.pending_output_leases == 0U);
    CHECK(controls->device_releases() == 1U);
}

TEST_CASE("gameinput.runtime.output_lease_affinity_fences_same_handle_reconnect",
    "[gameinput][runtime][rumble][affinity][concurrency][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());

    const auto device = handle(fixture.device_memory[0U]);
    const auto independent_device = handle(fixture.device_memory[1U]);
    fixture.connect(device);
    fixture.connect(independent_device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 2U);
    const auto old_target = fixture.connection(0U).target;
    const auto independent_target = fixture.connection(1U).target;

    auto old_lease_result = access->reserve(old_target);
    REQUIRE(old_lease_result.has_value());
    auto old_lease = std::move(*old_lease_result);
    auto old_endpoint_result = old_lease.create_rumble_endpoint();
    REQUIRE(old_endpoint_result.has_value());
    auto old_endpoint = std::move(*old_endpoint_result);

    const auto controls = fixture.native->rumble_script();
    controls->block_next_write();
    {
        std::jthread setter([&] {
            static_cast<void>(old_endpoint->set(output::rumble_value{
                .low_frequency = 0.5F,
                .high_frequency = 0.75F,
            }));
        });
        const scoped_rumble_release release_guard{*controls};
        REQUIRE(controls->wait_for_write());

        fixture.native->emit(device, native_device_event::disconnected);
        fixture.native->emit(device, native_device_event::connected);
        fixture.maintain();
        REQUIRE(fixture.connection_count() == 2U);
        const auto first_current_target = fixture.connection(0U).target;
        const auto second_current_target = fixture.connection(1U).target;
        const auto new_target = first_current_target == independent_target ? second_current_target
                                                                           : first_current_target;
        REQUIRE(new_target != independent_target);
        CHECK(new_target != old_target);
        CHECK(new_target.device != old_target.device);
        CHECK(fixture.native->retained_devices.load() == 3U);

        const auto factory_calls_before_refusal = controls->factory_calls();
        const auto blocked_new_lease = access->reserve(new_target);
        REQUIRE((!blocked_new_lease.has_value()));
        CHECK(blocked_new_lease.error().code == gameinput_error::resource_exhausted);
        CHECK(controls->factory_calls() == factory_calls_before_refusal);

        auto independent_lease_result = access->reserve(independent_target);
        REQUIRE(independent_lease_result.has_value());
        auto independent_lease = std::move(*independent_lease_result);
        auto independent_endpoint_result = independent_lease.create_rumble_endpoint();
        REQUIRE(independent_endpoint_result.has_value());
        auto independent_endpoint = std::move(*independent_endpoint_result);
        CHECK(independent_endpoint
                  ->set(output::rumble_value{
                      .low_frequency = 0.25F,
                      .high_frequency = 0.0F,
                  })
                  .status == output::native_status::accepted);
        independent_endpoint.reset();
        independent_lease = runtime_output_lease{};
        CHECK(controls->writes() == 1U);

        controls->release_write();
        setter.join();
        CHECK(controls->writes() == 2U);
    }

    controls->block_next_endpoint_release();
    {
        std::jthread endpoint_cleanup(
            [](std::unique_ptr<native_rumble_endpoint> endpoint) { endpoint.reset(); },
            std::move(old_endpoint));
        const scoped_rumble_release release_guard{*controls};
        REQUIRE(controls->wait_for_endpoint_release());

        const auto factory_calls_before_refusal = controls->factory_calls();
        const auto blocked_new_lease = access->reserve(
            fixture.connection(1U).target == independent_target ? fixture.connection(0U).target
                                                                : fixture.connection(1U).target);
        REQUIRE((!blocked_new_lease.has_value()));
        CHECK(blocked_new_lease.error().code == gameinput_error::resource_exhausted);
        CHECK(controls->factory_calls() == factory_calls_before_refusal);

        controls->release_endpoint_release();
        endpoint_cleanup.join();
    }

    const auto new_target = fixture.connection(1U).target == independent_target
                                ? fixture.connection(0U).target
                                : fixture.connection(1U).target;
    const auto factory_calls_before_refusal = controls->factory_calls();
    const auto blocked_without_endpoint = access->reserve(new_target);
    REQUIRE((!blocked_without_endpoint.has_value()));
    CHECK(blocked_without_endpoint.error().code == gameinput_error::resource_exhausted);
    CHECK(controls->factory_calls() == factory_calls_before_refusal);

    controls->block_next_device_release();
    {
        const scoped_rumble_release release_guard{*controls};
        old_lease = runtime_output_lease{};
        REQUIRE(controls->wait_for_device_release());
        CHECK(fixture.native->released_devices.load() == 0U);

        auto successor_lease_result = access->reserve(new_target);
        REQUIRE(successor_lease_result.has_value());
        auto successor_lease = std::move(*successor_lease_result);
        auto successor_endpoint_result = successor_lease.create_rumble_endpoint();
        REQUIRE(successor_endpoint_result.has_value());
        auto successor_endpoint = std::move(*successor_endpoint_result);
        CHECK(successor_endpoint
                  ->set(output::rumble_value{
                      .low_frequency = 0.75F,
                      .high_frequency = 0.5F,
                  })
                  .status == output::native_status::accepted);
        CHECK(controls->factory_calls() == 3U);

        successor_endpoint.reset();
        successor_lease = runtime_output_lease{};
        controls->release_device_release();
    }

    const auto release_deadline = std::chrono::steady_clock::now() + 2s;
    while (fixture.native->released_devices.load() == 0U &&
           std::chrono::steady_clock::now() < release_deadline) {
        std::this_thread::yield();
    }
    CHECK(fixture.native->released_devices.load() == 1U);

    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now + 8ms, drained));
    CHECK(drained.pending_output_leases == 0U);
    CHECK(drained.native_api_cleanup_complete);
    CHECK(controls->device_releases() == 3U);
}

} // namespace
