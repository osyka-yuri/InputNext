#include "rumble_window.hpp"

#include "manual_sequence.hpp"
#include "values/required_value.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

using inputnext::gameinput_detail::manual_sequence;
using inputnext::gameinput_detail::manual_step_result;
using inputnext::gameinput_detail::rumble_window;
using inputnext::test::required_value;

TEST_CASE("gameinput.probe.manual_window_gates_and_preserves_accepted_step_results",
    "[integration][gameinput][probe]") {
    constexpr auto title = L"InputNext manual window test";
    constexpr auto first_readiness = 11U;
    constexpr auto second_readiness = 12U;
    constexpr auto third_readiness = 13U;
    rumble_window window{{.title = title}};
    const HWND native = FindWindowW(nullptr, title);
    REQUIRE(native != nullptr);
    const HWND passed = GetDlgItem(native, 103);
    const HWND failed = GetDlgItem(native, 104);
    REQUIRE(passed != nullptr);
    REQUIRE(failed != nullptr);
    const auto click = [native](const WORD id) {
        SendMessageW(native, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
    };

    CHECK(IsWindowEnabled(passed) == FALSE);
    CHECK(IsWindowEnabled(failed) == FALSE);
    click(103);
    CHECK_FALSE(window.take_step_result().has_value());
    window.set_running(true);
    CHECK(IsWindowEnabled(GetDlgItem(native, 102)) != FALSE);
    CHECK_FALSE(window.abort_requested());

    window.enable_confirmation(first_readiness);
    CHECK(IsWindowEnabled(passed) != FALSE);
    CHECK(IsWindowEnabled(failed) != FALSE);
    const auto click_started = manual_sequence::clock::now();
    click(103);
    click(104);
    const auto first = required_value(window.take_step_result());
    CHECK(first.result == manual_step_result::passed);
    CHECK(first.readiness_id == first_readiness);
    CHECK(first.accepted_at >= click_started);
    CHECK_FALSE(window.take_step_result().has_value());

    window.enable_confirmation(second_readiness);
    click(104);
    window.disable_confirmation();
    CHECK(IsWindowEnabled(passed) == FALSE);
    CHECK(IsWindowEnabled(failed) == FALSE);
    click(103);
    const auto accepted_before_disable = required_value(window.take_step_result());
    CHECK(accepted_before_disable.result == manual_step_result::failed);
    CHECK(accepted_before_disable.readiness_id == second_readiness);
    CHECK_FALSE(window.take_step_result().has_value());

    window.enable_confirmation(third_readiness);
    click(104);
    const auto last = required_value(window.take_step_result());
    CHECK(last.result == manual_step_result::failed);
    CHECK(last.readiness_id == third_readiness);
    click(102);
    CHECK(window.abort_requested());
    window.set_running(false);
    CHECK(IsWindowEnabled(passed) == FALSE);
    CHECK(IsWindowEnabled(failed) == FALSE);
    window.close();
    CHECK_FALSE(window.valid());
}
