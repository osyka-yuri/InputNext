#include "xinput_report.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/xinput.hpp>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE("gameinput.probe.xinput_report_shows_current_process_and_focus_revision",
    "[unit][gameinput][probe][xinput]") {
    focus::snapshot snapshot{};
    snapshot.process.pid = 42U;
    snapshot.process.creation_time_100ns = 123456U;
    snapshot.revision = 9U;
    snapshot.state = focus::evidence::background;
    snapshot.open = true;
    snapshot.completed_observations = 12U;

    const auto report = format_xinput_focus(snapshot);

    CHECK(report.contains("state=background"));
    CHECK(report.contains("revision=9"));
    CHECK(report.contains("process_pid=42"));
    CHECK(report.contains("process_creation_100ns=123456"));
    CHECK(report.contains("observations=12"));
}

TEST_CASE("gameinput.probe.xinput_state_report_keeps_standard_and_extended_packets_distinct",
    "[unit][gameinput][probe][xinput]") {
    xinput::assignment assignment{};
    assignment.user_index = 2U;
    assignment.id = 17U;
    assignment.target.runtime.bytes[0U] = std::byte{0xAB};
    assignment.target.device.value = 31U;
    assignment.target.connection.value = 73U;
    focus::snapshot focus_snapshot{};
    focus_snapshot.revision = 8U;
    focus_snapshot.state = focus::evidence::foreground;
    XINPUT_STATE standard{};
    standard.dwPacketNumber = 4U;
    XINPUT_STATE extended{};
    extended.dwPacketNumber = 5U;
    extended.Gamepad.wButtons = 0x0400U;

    const auto report = format_xinput_state(assignment,
        99U,
        focus_snapshot,
        ERROR_SUCCESS,
        standard,
        ERROR_SUCCESS,
        extended);

    CHECK(report.contains("user=2 assignment=17"));
    CHECK(report.contains("runtime=ab000000000000000000000000000000"));
    CHECK(report.contains("key=31 connection=73"));
    CHECK(report.contains("focus=foreground focus_revision=8 latest_revision=99"));
    CHECK(report.contains("standard_packet=4 standard_neutral=true"));
    CHECK(report.contains("extended_packet=5 extended_neutral=false"));
}

} // namespace
} // namespace inputnext::gameinput_detail
