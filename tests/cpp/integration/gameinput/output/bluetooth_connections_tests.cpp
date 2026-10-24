#include "connection_test_support.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/device_metadata.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace inputnext::gameinput_detail {
namespace {

using namespace test_support;

TEST_CASE("gameinput.output.sony_bluetooth_keeps_input_without_opening_output",
    "[gameinput][output][integration][bluetooth][selection]") {
    for (const auto model :
        {metadata_device_model::dualsense, metadata_device_model::dualsense_edge}) {
        for (const auto motors : {0U, 1U, 2U, 3U}) {
            CAPTURE(model, motors);
            connection_fixture fixture;
            configure_dualsense(fixture, model, physical_transport::bluetooth);
            REQUIRE(fixture.start(false, true));
            auto& capabilities = fixture.backing.native->device_facts.gameinput_capabilities.value;
            capabilities.low_frequency_rumble = (motors & 1U) != 0U;
            capabilities.high_frequency_rumble = (motors & 2U) != 0U;
            fixture.backing.native->emit(fixture.device, native_device_event::haptic_info_updated);
            fixture.backing.maintain();

            const auto generic = checked_value(fixture.add_generic_connection());
            REQUIRE(fixture.connections->synchronize(fixture.backing.runtime->connections())
                    .has_value());
            // Generic admission follows Sony qualification on the same control worker.
            REQUIRE(fixture.wait_for_backend(generic));
            CHECK((!fixture.connections->backend_progress(fixture.target).has_value()));
            CHECK(fixture.controls->factory_calls() == 1U);
            CHECK(fixture.raw_controls->factory_calls() == 0U);
            CHECK(fixture.raw_controls->send_calls() == 0U);
            CHECK(fixture.backing.registry.capture(fixture.target).has_value());
            CHECK(fixture.backing.metadata->query_calls() >= 1U);

            fixture.connections->shutdown();
            CHECK(fixture.connections->progress().all_retirements_acknowledged);
        }
    }
}

} // namespace
} // namespace inputnext::gameinput_detail
