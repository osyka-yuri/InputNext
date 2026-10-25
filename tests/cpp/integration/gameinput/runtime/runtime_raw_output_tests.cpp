#include "gameinput/dualsense/usb_sink.hpp"

#include "gameinput/dualsense/usb_report.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "runtime_history_test_support.hpp"
#include "runtime_raw_report_test_support.hpp"
#include "runtime_test_support.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::gameinput_detail::dualsense;
using namespace inputnext::gameinput_test_support;

[[nodiscard]] output::native_command raw_command(const connection_target target,
    const output::channel_values values) {
    return output::native_command{
        .backend = output::backend_handle{.target = target},
        .output =
            output::native_output{
                .channels = output::supported_channel_mask,
                .values = values,
            },
    };
}

[[nodiscard]] output::channel_values sample_values() {
    return output::channel_values{
        .left_trigger =
            output::trigger_value{
                .mode = output::trigger_mode::feedback,
                .layout = output::trigger_layout::uniform,
                .start = 0.25F,
                .strength = 0.75F,
            },
        .right_trigger =
            output::trigger_value{
                .mode = output::trigger_mode::weapon,
                .layout = output::trigger_layout::uniform,
                .start = 0.25F,
                .end = 0.75F,
                .strength = 0.5F,
            },
        .rumble = output::rumble_value{.low_frequency = 0.25F, .high_frequency = 0.75F},
        .leds =
            output::led_value{
                .rgb = {0x10U, 0x20U, 0x30U},
                .player_leds = 3U,
                .microphone_led = true,
            },
    };
}

[[nodiscard]] bool wait_for_runtime_drain(gameinput_runtime& runtime,
    const maintenance_time now,
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

TEST_CASE("gameinput.runtime.dualsense_usb_sink_qualifies_metadata_snapshot",
    "[gameinput][runtime][raw_output][metadata][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;

    const auto description = access->describe(target);
    REQUIRE(description.has_value());
    REQUIRE(description->model.has_value());
    CHECK(description->model.value_or(metadata_device_model::generic_gamepad) ==
          metadata_device_model::dualsense);

    fixture.metadata->model = metadata_field<metadata_device_model>{
        .availability = metadata_availability::available,
        .value = metadata_device_model::dualsense,
    };
    fixture.metadata->transport = metadata_field<transport_metadata>{
        .availability = metadata_availability::available,
        .value =
            transport_metadata{
                .value = physical_transport::usb,
                .provenance = transport_provenance::native_bus_type,
            },
    };

    const auto qualified = access->qualify_dualsense_usb(target, description->catalogue_generation);
    REQUIRE(qualified.has_value());
    CHECK(*qualified);

    const auto stale =
        access->qualify_dualsense_usb(target, description->catalogue_generation + 1U);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == metadata_error::stale);

    fixture.metadata->transport.value.provenance = transport_provenance::pnp_path_heuristic;
    const auto heuristic = access->qualify_dualsense_usb(target, description->catalogue_generation);
    REQUIRE(heuristic.has_value());
    CHECK((!*heuristic));

    fixture.metadata->transport.value.provenance = transport_provenance::native_enumerator;
    fixture.metadata->conflicting_identity = true;
    const auto conflicting =
        access->qualify_dualsense_usb(target, description->catalogue_generation);
    REQUIRE(conflicting.has_value());
    CHECK((!*conflicting));

    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
}

TEST_CASE("gameinput.runtime.dualsense_usb_sink_selects_exact_report_payload_capacity",
    "[gameinput][runtime][raw_output][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    const auto controls = fixture.native->raw_report_script();
    const auto values = sample_values();
    const auto image = encode_usb_report(values);
    constexpr std::array capacities{64U, 63U, 48U, 47U, 49U};

    std::size_t expected_fill_calls{};
    std::size_t expected_send_calls{};
    for (std::size_t index = 0U; index < capacities.size(); ++index) {
        const auto capacity = capacities[index];
        controls->set_capacity(capacity);
        auto lease = access->reserve(target);
        REQUIRE(lease.has_value());
        auto sink_result = make_dualsense_usb_sink(std::move(*lease));
        REQUIRE(sink_result.has_value());
        auto sink = std::move(*sink_result);
        CHECK(controls->factory_calls() == index);

        auto wrong_target = raw_command(target, values);
        wrong_target.backend.target.connection.value += 1U;
        CHECK(sink->write(wrong_target).status == output::native_status::disconnected);
        auto wrong_mask = raw_command(target, values);
        wrong_mask.output.channels = output::rumble_mask;
        CHECK(sink->write(wrong_mask).status == output::native_status::unsupported);
        auto unexpected_hd = raw_command(target, values);
        unexpected_hd.output.body =
            output::body_route_intent{.route = output::body_actuator_route::hd};
        CHECK(sink->write(unexpected_hd).status == output::native_status::unsupported);
        CHECK(controls->factory_calls() == index);

        const auto result = sink->write(raw_command(target, values));
        CHECK(controls->factory_calls() == index + 1U);
        const auto payload = usb_report_payload(image, capacity);
        if (payload.has_value()) {
            CHECK(result.status == output::native_status::accepted);
            ++expected_fill_calls;
            ++expected_send_calls;
            const auto fills = controls->fill_payloads();
            const auto sends = controls->sent_payloads();
            REQUIRE(fills.size() == expected_fill_calls);
            REQUIRE(sends.size() == expected_send_calls);
            CHECK((fills.back() == std::vector<std::byte>(payload->begin(), payload->end())));
            CHECK((sends.back() == std::vector<std::byte>(payload->begin(), payload->end())));
        } else {
            CHECK(result.status == output::native_status::unsupported);
            CHECK(controls->fill_calls() == expected_fill_calls);
            CHECK(controls->send_calls() == expected_send_calls);
        }
        sink.reset();
    }

    CHECK((controls->requested_report_ids() == std::vector<std::uint32_t>{2U, 2U, 2U, 2U, 2U}));
    CHECK(controls->report_releases() == capacities.size());
    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
}

TEST_CASE("gameinput.runtime.dualsense_usb_sink_lazily_retries_report_creation",
    "[gameinput][runtime][raw_output][rollback][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    const auto controls = fixture.native->raw_report_script();
    auto lease = access->reserve(target);
    REQUIRE(lease.has_value());
    auto sink_result = make_dualsense_usb_sink(std::move(*lease));
    REQUIRE(sink_result.has_value());
    auto sink = std::move(*sink_result);
    const auto command = raw_command(target, sample_values());

    controls->fail_next_factory(output::native_result{
        .status = output::native_status::retryable_failure,
        .native_code = 0x1234,
    });
    const auto failed_create = sink->write(command);
    CHECK((failed_create == output::native_result{
                                .status = output::native_status::retryable_failure,
                                .native_code = 0x1234,
                            }));
    CHECK(controls->fill_calls() == 0U);
    CHECK(controls->send_calls() == 0U);

    controls->return_null_next_factory();
    const auto null_create = sink->write(command);
    CHECK((null_create == output::native_result{
                              .status = output::native_status::retryable_failure,
                          }));
    CHECK(controls->report_creations() == 0U);

    CHECK(sink->write(command).status == output::native_status::accepted);
    CHECK(controls->factory_calls() == 3U);
    CHECK(controls->report_creations() == 1U);
    CHECK(controls->fill_calls() == 1U);
    CHECK(controls->send_calls() == 1U);
    CHECK((controls->requested_report_ids() == std::vector<std::uint32_t>{2U, 2U, 2U}));

    sink.reset();
    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
}

TEST_CASE("gameinput.runtime.dualsense_usb_sink_refills_after_fill_and_send_failures",
    "[gameinput][runtime][raw_output][retry][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    const auto controls = fixture.native->raw_report_script();
    auto lease = access->reserve(target);
    REQUIRE(lease.has_value());
    auto sink_result = make_dualsense_usb_sink(std::move(*lease));
    REQUIRE(sink_result.has_value());
    auto sink = std::move(*sink_result);
    const auto values = sample_values();
    const auto command = raw_command(target, values);
    const auto image = encode_usb_report(values);

    controls->fail_next_fill();
    CHECK((sink->write(command) == output::native_result{
                                       .status = output::native_status::retryable_failure,
                                   }));
    CHECK(controls->factory_calls() == 1U);
    CHECK(controls->send_calls() == 0U);

    CHECK(sink->write(command).status == output::native_status::accepted);
    controls->send_next_with(output::native_result{
        .status = output::native_status::disconnected,
        .native_code = 0x838A0001,
    });
    CHECK((sink->write(command) == output::native_result{
                                       .status = output::native_status::disconnected,
                                       .native_code = 0x838A0001,
                                   }));
    CHECK(sink->write(raw_command(target,
                          output::channel_values{
                              .rumble = {.low_frequency = 1.0F, .high_frequency = 0.0F}}))
              .status == output::native_status::accepted);

    CHECK(controls->factory_calls() == 1U);
    CHECK(controls->report_creations() == 1U);
    CHECK(controls->fill_calls() == 4U);
    CHECK(controls->send_calls() == 3U);
    const auto fills = controls->fill_payloads();
    REQUIRE(fills.size() == 4U);
    CHECK((fills[0U] == std::vector<std::byte>(image.begin(), image.end())));
    CHECK((fills[1U] == std::vector<std::byte>(image.begin(), image.end())));
    CHECK((fills[2U] == std::vector<std::byte>(image.begin(), image.end())));
    const auto latest_image = encode_usb_report(
        output::channel_values{.rumble = {.low_frequency = 1.0F, .high_frequency = 0.0F}});
    CHECK((fills[3U] == std::vector<std::byte>(latest_image.begin(), latest_image.end())));

    sink.reset();
    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
}

TEST_CASE("gameinput.runtime.dualsense_usb_report_dies_before_lease_and_native_owner",
    "[gameinput][runtime][raw_output][lifetime][integration]") {
    runtime_fixture fixture;
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    const auto controls = fixture.native->raw_report_script();
    auto lease = access->reserve(target);
    REQUIRE(lease.has_value());
    auto sink_result = make_dualsense_usb_sink(std::move(*lease));
    REQUIRE(sink_result.has_value());
    CHECK(sink_result->get()->write(raw_command(target, sample_values())).status ==
          output::native_status::accepted);
    controls->block_next_report_release();

    std::jthread destroyer{[sink = std::move(*sink_result)] mutable { sink.reset(); }};
    const scoped_raw_report_release release_guard{*controls};
    REQUIRE(controls->wait_for_report_release());
    const auto pending = fixture.runtime->stop(fixture.now);
    CHECK((!pending.drained));
    CHECK(pending.pending_output_leases == 1U);
    CHECK(controls->device_releases() == 0U);
    CHECK(controls->send_calls() == 1U);

    controls->release_report_release();
    destroyer.join();
    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, drained));
    CHECK(drained.pending_output_leases == 0U);
    CHECK(controls->report_releases() == 1U);
    CHECK(controls->report_release_order() != 0U);
    CHECK(controls->device_release_order() > controls->report_release_order());
}

TEST_CASE("gameinput.runtime.blocked_usb_output_keeps_input_and_history_progress",
    "[gameinput][runtime][raw_output][concurrency][integration]") {
    runtime_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    fixture.native->configure_history(history);
    auto access_result = runtime_output_access::create(*fixture.runtime);
    REQUIRE(access_result.has_value());
    const auto access = std::move(*access_result);
    REQUIRE(fixture.start().has_value());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.connect(device);
    fixture.maintain();
    const auto target = fixture.connection().target;
    auto history_reader = fixture.runtime->open_history(target, fixture.now);
    REQUIRE(history_reader.has_value());
    const auto baseline = fixture.runtime->read_history(*history_reader, fixture.now);
    REQUIRE(baseline.count == 1U);

    auto lease = access->reserve(target);
    REQUIRE(lease.has_value());
    auto sink_result = make_dualsense_usb_sink(std::move(*lease));
    REQUIRE(sink_result.has_value());
    auto sink = std::move(*sink_result);
    const auto controls = fixture.native->raw_report_script();
    controls->block_next_send();
    std::optional<output::native_result> write_result;
    std::jthread writer([&] { write_result = sink->write(raw_command(target, sample_values())); });
    const scoped_raw_report_release release_guard{*controls};
    REQUIRE(controls->wait_for_send());

    fixture.native->publish(device, pad_parts(test_pad(8U), 120U, 1'200U));
    const auto latest = fixture.registry.read(target);
    REQUIRE(latest.has_value());
    CHECK(latest->sample.state.buttons == 8U);

    history->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = test_pad(16U),
                .source_timestamp_us = 125U,
                .observation_qpc = 1'250U,
            },
    });
    const auto history_progress = fixture.runtime->read_history(*history_reader, fixture.now);
    REQUIRE(history_progress.count == 1U);
    CHECK(history_progress.records[0U].sample.state.buttons == 16U);
    CHECK(fixture.native->released_devices.load(std::memory_order_relaxed) == 0U);

    CHECK(fixture.runtime->close_history(*history_reader).has_value());
    const auto pending = fixture.runtime->stop(fixture.now);
    CHECK((!pending.drained));
    CHECK(pending.pending_output_leases == 1U);

    controls->release_send();
    writer.join();
    REQUIRE(write_result.has_value());
    CHECK(write_result.value_or(output::native_result{.status = output::native_status::failed})
              .status == output::native_status::accepted);
    sink.reset();
    gameinput_stop_report drained{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, drained));
    CHECK(drained.pending_output_leases == 0U);
}

TEST_CASE("gameinput.runtime.same_handle_reconnect_waits_for_raw_report_lease",
    "[gameinput][runtime][raw_output][affinity][concurrency][integration]") {
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
    auto old_lease = access->reserve(old_target);
    REQUIRE(old_lease.has_value());
    auto old_sink_result = make_dualsense_usb_sink(std::move(*old_lease));
    REQUIRE(old_sink_result.has_value());
    auto old_sink = std::move(*old_sink_result);
    const auto controls = fixture.native->raw_report_script();
    controls->block_next_send();
    std::optional<output::native_result> write_result;
    std::jthread writer(
        [&] { write_result = old_sink->write(raw_command(old_target, sample_values())); });
    const scoped_raw_report_release release_guard{*controls};
    REQUIRE(controls->wait_for_send());

    fixture.native->emit(device, native_device_event::disconnected);
    fixture.native->emit(device, native_device_event::connected);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 2U);
    const auto first_current_target = fixture.connection(0U).target;
    const auto second_current_target = fixture.connection(1U).target;
    const auto new_target =
        first_current_target == independent_target ? second_current_target : first_current_target;
    REQUIRE(new_target != independent_target);
    CHECK(new_target != old_target);
    CHECK(new_target.device != old_target.device);

    const auto factory_calls_before_refusal = controls->factory_calls();
    const auto blocked_lease = access->reserve(new_target);
    REQUIRE_FALSE(blocked_lease.has_value());
    CHECK(blocked_lease.error().code == gameinput_error::resource_exhausted);
    CHECK(controls->factory_calls() == factory_calls_before_refusal);

    controls->release_send();
    writer.join();
    REQUIRE(write_result.has_value());
    CHECK(write_result.value_or(output::native_result{.status = output::native_status::failed})
              .status == output::native_status::accepted);
    old_sink.reset();

    auto next_lease = access->reserve(new_target);
    REQUIRE(next_lease.has_value());
    auto next_sink_result = make_dualsense_usb_sink(std::move(*next_lease));
    REQUIRE(next_sink_result.has_value());
    auto next_sink = std::move(*next_sink_result);
    CHECK(next_sink->write(raw_command(new_target, sample_values())).status ==
          output::native_status::accepted);
    CHECK(controls->factory_calls() == factory_calls_before_refusal + 1U);
    CHECK(controls->report_releases() == 1U);

    next_sink.reset();
    gameinput_stop_report stopped{};
    CHECK(wait_for_runtime_drain(*fixture.runtime, fixture.now, stopped));
    CHECK(stopped.pending_output_leases == 0U);
    CHECK(stopped.native_api_cleanup_complete);
}

} // namespace
