#include "gameinput_probe/input/xinput/event_report.hpp"

#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>

namespace inputnext::gameinput_detail {
namespace {

TEST_CASE("gameinput.probe.xinput_event_report_formats_keystrokes_and_empty_drains",
    "[unit][gameinput][probe][xinput][events]") {
    xinput::assignment assignment{};
    assignment.user_index = 2U;
    assignment.id = 17U;
    XINPUT_KEYSTROKE key{};
    key.VirtualKey = 0x5800U;
    key.Unicode = L'A';
    key.Flags = XINPUT_KEYSTROKE_KEYDOWN;
    key.UserIndex = static_cast<BYTE>(assignment.user_index);

    const auto event = format_xinput_keystroke(assignment, ERROR_SUCCESS, &key);
    const auto empty = format_xinput_keystroke_drain({.user_index = assignment.user_index,
        .result = ERROR_EMPTY,
        .drained = 0U,
        .limit_reached = false});

    CHECK(event.contains("user=2 assignment=17 result=success code=0x00000000"));
    CHECK(event.contains("virtual_key=0x5800 unicode=0x0041 flags=0x0001 event_user=2"));
    CHECK(event.contains("event_user=2"));
    CHECK(empty.contains("user=2 result=empty code=0x"));
    CHECK(empty.contains("drained=0 limit_reached=false"));
}

TEST_CASE("gameinput.probe.xinput_event_report_keeps_guide_outcomes_typed",
    "[unit][gameinput][probe][xinput][events]") {
    const xinput::guide_wait_ticket ticket{.user_index = 1U, .id = 23U};
    const xinput::guide_wait_result<xinput::guide_wait_ticket> admitted{ticket};
    const xinput::guide_wait_result<void> completed{};
    const xinput::guide_wait_result<void> cancelled{
        std::unexpected(xinput::guide_wait_error::cancelled)};
    const xinput::guide_wait_result<bool> cancel_succeeded{true};

    CHECK(format_xinput_guide_begin(ticket.user_index, admitted)
            .contains("result=admitted ticket=23"));
    CHECK(format_xinput_guide_wait(ticket, completed).contains("result=completed"));
    CHECK(format_xinput_guide_wait(ticket, cancelled).contains("result=error error=cancelled"));
    CHECK(format_xinput_guide_cancel(ticket, cancel_succeeded).contains("result=true"));
}

} // namespace
} // namespace inputnext::gameinput_detail
