#include "connection_test_support.hpp"
#include <inputnext/output_body.hpp>

#include "cpp/integration/gameinput/runtime/runtime_metadata_test_support.hpp"
#include "gameinput/dualsense/usb_report.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

using namespace test_support;

void configure_usb(connection_fixture& fixture,
    const metadata_device_model model = metadata_device_model::dualsense) {
    configure_dualsense(fixture, model);
}

void refresh_facts(connection_fixture& fixture) {
    fixture.backing.native->emit(fixture.device, native_device_event::haptic_info_updated);
    fixture.backing.maintain();
}

[[nodiscard]] bool has_route(const connection_fixture& fixture,
    const output::channel_mask channels) {
    const auto progress = fixture.connections->backend_progress(fixture.target);
    return progress.has_value() && progress->phase == output::backend_phase::current &&
           progress->represented_channels == channels;
}

TEST_CASE("gameinput.output.usb_backend_owns_all_channels_without_gameinput_motors",
    "[gameinput][output][integration][dualsense][selection]") {
    for (const auto model :
        {metadata_device_model::dualsense, metadata_device_model::dualsense_edge}) {
        connection_fixture fixture;
        configure_usb(fixture, model);
        REQUIRE(fixture.start(false));
        fixture.backing.native->device_facts.gameinput_capabilities.value.low_frequency_rumble =
            false;
        fixture.backing.native->device_facts.gameinput_capabilities.value.high_frequency_rumble =
            false;
        refresh_facts(fixture);
        REQUIRE(
            fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
        REQUIRE(wait_until([&] { return has_route(fixture, output::supported_channel_mask); }));
        REQUIRE(wait_until([&] { return fixture.raw_controls->send_calls() > 0U; }));
        CHECK(fixture.controls->factory_calls() == 0U);
        CHECK(fixture.controls->writes() == 0U);
        CHECK(fixture.backing.metadata->query_calls() >= 1U);
        const auto backend =
            checked_value(fixture.connections->backend_progress(fixture.target)).backend;
        REQUIRE(wait_until([&] { return fixture.backing.metadata->completed_queries() >= 3U; }));
        CHECK(checked_value(fixture.connections->backend_progress(fixture.target)).backend ==
              backend);
        fixture.connections->shutdown();
        CHECK(fixture.connections->progress().all_retirements_acknowledged);
    }
}

TEST_CASE("gameinput.output.usb_selection_requires_authoritative_nonconflicting_transport",
    "[gameinput][output][integration][dualsense][selection]") {
    for (const auto scenario : {0U, 1U, 2U, 3U, 4U, 5U}) {
        connection_fixture fixture;
        configure_usb(fixture);
        switch (scenario) {
        case 0U:
            fixture.backing.metadata->transport.value.value = physical_transport::bluetooth;
            break;
        case 1U:
            fixture.backing.metadata->transport.value.provenance =
                transport_provenance::pnp_path_heuristic;
            break;
        case 2U:
            fixture.backing.metadata->transport.availability = metadata_availability::ambiguous;
            break;
        case 3U:
            fixture.backing.metadata->conflicting_identity = true;
            break;
        case 4U:
            fixture.backing.metadata->transport.availability = metadata_availability::unavailable;
            break;
        default:
            fixture.backing.native->device_facts.vendor_id.value = 0x045EU;
            break;
        }
        REQUIRE(fixture.start(false));
        const auto generic = checked_value(fixture.add_generic_connection());
        REQUIRE(
            fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
        // The later generic target proves the worker has processed the earlier Sony target.
        REQUIRE(fixture.wait_for_backend(generic));
        if (scenario == 5U) {
            REQUIRE(wait_until([&] { return has_route(fixture, output::rumble_mask); }));
        } else {
            CHECK((!fixture.connections->backend_progress(fixture.target).has_value()));
            CHECK(fixture.controls->factory_calls() == 1U);
        }
        CHECK(fixture.raw_controls->factory_calls() == 0U);
        if (scenario == 5U) {
            CHECK(fixture.backing.metadata->query_calls() == 0U);
        } else {
            CHECK(fixture.backing.metadata->query_calls() >= 1U);
        }
    }
}

TEST_CASE("gameinput.output.usb_qualification_failure_stays_pending_then_recovers",
    "[gameinput][output][integration][dualsense][recovery]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));
    fixture.backing.metadata->make_next_query_busy();
    fixture.backing.metadata->block_next_queries(1U);
    scoped_query_release release{*fixture.backing.metadata, 1U};
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_until([&] {
        const auto report = fixture.connections->progress();
        return report.failure.has_value() &&
               report.failure->operation == connection_control_operation::qualify_backend &&
               report.failure->metadata_failure == metadata_error::busy;
    }));
    CHECK(fixture.controls->factory_calls() == 0U);
    CHECK(fixture.raw_controls->factory_calls() == 0U);
    REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));
    CHECK((!fixture.connections->backend_progress(fixture.target).has_value()));
    release.release();
    REQUIRE(wait_until([&] { return has_route(fixture, output::supported_channel_mask); }));
    CHECK(fixture.backing.metadata->query_calls() >= 2U);
}

TEST_CASE("gameinput.output.stale_usb_qualification_cannot_admit_the_old_generation",
    "[gameinput][output][integration][dualsense][generation]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));
    fixture.backing.metadata->block_next_queries(1U);
    scoped_query_release release{*fixture.backing.metadata, 1U};
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.backing.metadata->wait_for_blocked(1U));
    fixture.backing.native->device_facts.vendor_id.value = 0x045EU;
    refresh_facts(fixture);
    release.release();
    REQUIRE(wait_until([&] { return has_route(fixture, output::rumble_mask); }));
    CHECK(fixture.raw_controls->factory_calls() == 0U);
    const auto failure = checked_value(fixture.connections->progress().failure);
    CHECK(failure.metadata_failure == metadata_error::stale);
}

TEST_CASE("gameinput.output.route_change_waits_for_raw_send_and_report_release",
    "[gameinput][output][integration][dualsense][retirement][concurrency]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));
    fixture.raw_controls->block_next_send();
    fixture.raw_controls->block_next_report_release();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.raw_controls->wait_for_send());
    REQUIRE(wait_until([&] { return has_route(fixture, output::supported_channel_mask); }));
    const auto old_backend =
        checked_value(fixture.connections->backend_progress(fixture.target)).backend;
    fixture.backing.native->device_facts.vendor_id.value = 0x045EU;
    refresh_facts(fixture);
    REQUIRE(wait_until([&] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::retiring;
    }));
    CHECK(fixture.controls->factory_calls() == 0U);
    CHECK(fixture.connections->progress().backend_obligations == 1U);
    fixture.raw_controls->release_send();
    REQUIRE(fixture.raw_controls->wait_for_report_release());
    CHECK(fixture.controls->factory_calls() == 0U);
    CHECK(fixture.connections->progress().pending_retirements == 1U);
    fixture.raw_controls->release_report_release();
    REQUIRE(wait_until([&] { return has_route(fixture, output::rumble_mask); }));
    const auto replacement = checked_value(fixture.connections->backend_progress(fixture.target));
    CHECK(replacement.backend != old_backend);
    CHECK(fixture.raw_controls->factory_calls() == 1U);
    CHECK(fixture.raw_controls->send_calls() == 1U);
    CHECK(fixture.connections->progress().backend_obligations == 1U);
    CHECK(fixture.connections->progress().pending_retirements == 0U);
}

TEST_CASE("gameinput.output.replaced_desire_cancels_the_old_route_candidate",
    "[gameinput][output][integration][dualsense][admission][concurrency]") {
    connection_fixture fixture;
    configure_usb(fixture);
    fixture.backing.native->device_facts.vendor_id.value = 0x045EU;
    REQUIRE(fixture.start(false));
    fixture.controls->block_next_factory();
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(fixture.controls->wait_for_factory());
    REQUIRE(fixture.connections->synchronize(connection_inventory{}).has_value());
    fixture.backing.native->device_facts.vendor_id.value = 0x054CU;
    refresh_facts(fixture);
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    fixture.controls->release_factory();
    REQUIRE(wait_until([&] { return has_route(fixture, output::supported_channel_mask); }));
    CHECK(fixture.controls->writes() == 0U);
    CHECK(fixture.controls->factory_calls() == 1U);
    CHECK(fixture.connections->progress().backend_obligations == 1U);
}

TEST_CASE("gameinput.output.usb_backend_retries_failed_fill_and_send_autonomously",
    "[gameinput][output][integration][dualsense][retry]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));
    fixture.raw_controls->fail_next_fill();
    fixture.raw_controls->send_next_with(
        {.status = output::native_status::retryable_failure, .native_code = 42});
    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_until([&] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() &&
               progress->delivery == output::delivery_state::setter_accepted &&
               !progress->write_in_flight;
    }));
    CHECK(fixture.raw_controls->factory_calls() == 1U);
    CHECK(fixture.raw_controls->fill_calls() == 3U);
    CHECK(fixture.raw_controls->send_calls() == 2U);
    CHECK(fixture.controls->writes() == 0U);
}

TEST_CASE("gameinput.output.usb_backend_delivers_arbitrated_values_and_live_neutral",
    "[gameinput][output][integration][dualsense][delivery]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start());
    REQUIRE(fixture.wait_for_backend(fixture.target));
    auto& authority = fixture.composition->public_authority();
    const auto context = authority.open(fixture.target);
    REQUIRE(context.has_value());
    const output::channel_values values{.left_trigger = {.mode = output::trigger_mode::feedback,
                                            .start = 0.25F,
                                            .end = 1.0F,
                                            .strength = 0.5F},
        .right_trigger = {.mode = output::trigger_mode::weapon,
            .start = 0.3F,
            .end = 0.7F,
            .strength = 0.75F},
        .rumble = {.low_frequency = 0.25F, .high_frequency = 0.75F},
        .leds = {.rgb = {0x20U, 0x40U, 0x80U}, .player_leds = 3U, .microphone_led = true}};
    const auto submitted = authority.submit(*context,
        {.selected_channels = output::supported_channel_mask,
            .sequence = 1U,
            .ttl = std::chrono::seconds{5},
            .values = values});
    REQUIRE(submitted.has_value());
    const auto delivered = [&](const output::channel_values& expected) {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        if (!progress.has_value() || progress->phase != output::backend_phase::current ||
            progress->write_in_flight || !progress->last_completed_attempt.has_value()) {
            return false;
        }
        const auto& attempt = *progress->last_completed_attempt;
        return progress->last_attempt == attempt.id &&
               attempt.publication == progress->observed_publication &&
               attempt.result.status == output::native_status::accepted &&
               attempt.output.channels == output::supported_channel_mask &&
               attempt.output.values == expected;
    };
    REQUIRE(wait_until([&] { return delivered(values); }));
    const auto image = dualsense::encode_usb_report(values);
    const auto sent = fixture.raw_controls->sent_payloads();
    REQUIRE(!sent.empty());
    CHECK((sent.back() == std::vector<std::byte>(image.begin(), image.end())));
    CHECK(fixture.controls->writes() == 0U);

    const auto closed = authority.close(*context, context->identity.id.value);
    REQUIRE(closed.has_value());
    REQUIRE(authority.acknowledge(*closed).has_value());
    REQUIRE(wait_until([&] { return delivered(output::channel_values{}); }));
    const auto neutral = dualsense::encode_usb_report(output::channel_values{});
    const auto final_payloads = fixture.raw_controls->sent_payloads();
    REQUIRE(!final_payloads.empty());
    CHECK((final_payloads.back() == std::vector<std::byte>(neutral.begin(), neutral.end())));
}

TEST_CASE("gameinput.output.qualified_usb_backend_admits_hd_route_and_sets_hd_mode",
    "[gameinput][output][integration][dualsense_usb][body]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));
    auto audio_routes = fixture.composition->audio_routes();
    auto audio = audio_routes.attach(fixture.target, output::body_route_support::hd, true);
    REQUIRE(audio.has_value());

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    REQUIRE(wait_until([&] { return has_route(fixture, output::supported_channel_mask); }));
    auto& authority = fixture.composition->public_authority();
    auto context = authority.open(fixture.target);
    REQUIRE(context.has_value());
    const auto body = authority.submit_body(*context,
        output::body_request{.audio = *audio,
            .sequence = 1U,
            .ttl = std::chrono::seconds{5},
            .preference = output::body_route_preference::prefer_hd,
            .fallback = {.low_frequency = 0.4F, .high_frequency = 0.6F}});
    REQUIRE(body.has_value());
    fixture.output_executor->request_drive();

    REQUIRE(wait_until([&] {
        const auto progress = fixture.connections->backend_progress(fixture.target);
        return progress.has_value() && progress->phase == output::backend_phase::current &&
               progress->acknowledged_body_route.has_value() &&
               progress->acknowledged_body_route->intent.route == output::body_actuator_route::hd;
    }));
    const auto sent = fixture.raw_controls->sent_payloads();
    REQUIRE_FALSE(sent.empty());
    const auto expected =
        dualsense::encode_usb_report(output::channel_values{}, output::body_actuator_route::hd);
    CHECK((sent.back() == std::vector<std::byte>(expected.begin(), expected.end())));

    const auto closed = authority.close(*context, context->identity.id.value);
    REQUIRE(closed.has_value());
    REQUIRE(authority.acknowledge(*closed).has_value());
    REQUIRE(audio_routes.detach(*audio).has_value());
}

TEST_CASE("gameinput.output.dualsense_usb.backend_admission_waits_for_explicit_start",
    "[gameinput][output][integration][dualsense_usb]") {
    connection_fixture fixture;
    configure_usb(fixture);
    REQUIRE(fixture.start(false));

    const auto raw_reports = fixture.backing.native->raw_report_script();
    REQUIRE(raw_reports != nullptr);
    CHECK(raw_reports->factory_calls() == 0U);
    CHECK(raw_reports->send_calls() == 0U);

    REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections()).has_value());
    CHECK(wait_until([&raw_reports] { return raw_reports->send_calls() != 0U; }));
}

} // namespace
} // namespace inputnext::gameinput_detail
