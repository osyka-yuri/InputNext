#include "catalogue_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/exclusive/lease.hpp"
#include "exclusive_native_test_support.hpp"
#include "support/focus/manual_clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_catalogue_test;
using namespace inputnext::dinput_native_test_support;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] bool mark_selected_device_polled(native_service_fixture& fixture) {
    std::scoped_lock lock{fixture.catalogue.system->gate};
    if (fixture.catalogue.system->devices.empty()) {
        return false;
    }
    fixture.catalogue.system->devices.back().polled = true;
    return true;
}

} // namespace

TEST_CASE("dinput.exclusive_native_background_requires_initial_proof",
    "[integration][dinput][exclusive][native][background]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    HRESULT result = DI_OK;
    HRESULT expected = DI_OK;
    bool delayed{};

    SECTION("no state available") {
        result = E_PENDING;
        expected = E_PENDING;
        delayed = true;
    }
    SECTION("native ownership revoked") {
        result = DIERR_NOTACQUIRED;
        expected = DIERR_NOTACQUIRED;
    }
    SECTION("native revocation returns after the prospective proof deadline") {
        result = DIERR_NOTACQUIRED;
        expected = DIERR_NOTACQUIRED;
        delayed = true;
    }
    SECTION("native input lost") {
        result = DIERR_INPUTLOST;
        expected = DIERR_INPUTLOST;
    }
    SECTION("successful probe completes at its deadline") {
        expected = DIERR_INPUTLOST;
        delayed = true;
    }

    if (delayed) {
        controls->block_next(operation);
    }
    controls->set_result(operation, result);
    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    if (delayed) {
        REQUIRE(controls->wait_for_entry(operation));
        clock->advance(250ms);
        controls->release_block(operation);
    }
    REQUIRE((*lease)->wait_prepared() == expected);
    CHECK((*lease)->authority_status() ==
          (expected == DIERR_NOTACQUIRED ? DIERR_NOTACQUIRED : DIERR_INPUTLOST));
    CHECK((*lease)->try_activate() == false);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::get_capabilities) == 1U);
    CHECK(controls->count(native_operation::poll) == 0U);
}

TEST_CASE("dinput.exclusive_native_background_pending_retry_commits_fresh_proof",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto state_operation = native_operation::get_device_state;
    constexpr auto poll_operation = native_operation::poll;
    controls->set_result_sequence(state_operation, {E_PENDING, DI_OK});
    controls->set_result(poll_operation, DI_NOEFFECT);
    controls->block_next(state_operation);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(state_operation));
    clock->advance(100ms);
    controls->release_block(state_operation, true);
    REQUIRE(controls->wait_for_entry(state_operation, 2U));
    controls->block_next(poll_operation);
    controls->release_block(state_operation);
    REQUIRE(controls->wait_for_entry(poll_operation));

    clock->advance(100ms);
    controls->release_block(poll_operation, true);
    REQUIRE(controls->wait_for_entry(poll_operation, 2U));
    clock->advance(101ms);
    CHECK((*lease)->authority_status() == DI_OK);
    CHECK((*lease)->try_activate());

    (*lease)->cancel();
    controls->release_block(poll_operation);
    REQUIRE((*lease)->wait_closed() == DI_OK);

    const auto observations = controls->observations();
    auto selected = std::views::filter(observations,
        [](const native_observation& item) { return item.selected_device; });
    const auto format = std::ranges::find_if(observations, [](const native_observation& item) {
        return item.selected_device && item.operation == native_operation::set_data_format;
    });
    const auto capabilities =
        std::ranges::find_if(observations, [](const native_observation& item) {
            return item.selected_device && item.operation == native_operation::get_capabilities;
        });
    REQUIRE(format != observations.end());
    REQUIRE(capabilities != observations.end());
    CHECK(format < capabilities);
    CHECK((capabilities->flags & DIDC_POLLEDDATAFORMAT) != 0U);

    std::size_t state_count{};
    std::size_t poll_count{};
    bool unconsumed_poll{};
    for (const auto& observation : selected) {
        if (observation.operation == poll_operation) {
            ++poll_count;
            CHECK(observation.result == DI_NOEFFECT);
            unconsumed_poll = true;
        } else if (observation.operation == state_operation) {
            ++state_count;
            CHECK(unconsumed_poll);
            unconsumed_poll = false;
        }
    }
    CHECK(state_count == 3U);
    CHECK(poll_count == 4U);
}

TEST_CASE("dinput.exclusive_native_background_wait_deadline_expires_during_blocked_retry",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    controls->set_result_sequence(operation, {E_PENDING, DI_OK});
    controls->block_next(operation);

    const auto lease = fixture.reserve_background(std::shared_ptr<test::focus::manual_clock>{});
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(operation));
    auto prepared = std::async(std::launch::async, [&lease] { return (*lease)->wait_prepared(); });
    controls->release_block(operation, true);
    const bool retry_entered = controls->wait_for_entry(operation, 2U);
    CHECK(retry_entered);
    const bool completed = prepared.wait_for(1s) == std::future_status::ready;
    CHECK(completed);
    if (!completed) {
        (*lease)->cancel();
        controls->release_block(operation);
    }
    const HRESULT prepared_result = prepared.get();
    CHECK(controls->count(operation) == 2U);
    controls->release_block(operation);
    CHECK(prepared_result == E_PENDING);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    CHECK((*lease)->try_activate() == false);
}

TEST_CASE("dinput.exclusive_native_background_repeated_pending_keeps_original_deadline",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    controls->set_result(operation, E_PENDING);
    controls->block_next(operation);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(operation));
    clock->advance(100ms);
    controls->release_block(operation, true);
    REQUIRE(controls->wait_for_entry(operation, 2U));
    clock->advance(100ms);
    controls->release_block(operation, true);
    REQUIRE(controls->wait_for_entry(operation, 3U));
    clock->advance(50ms);

    const HRESULT authority = (*lease)->authority_status();
    CHECK(authority == DIERR_INPUTLOST);
    if (authority != DIERR_INPUTLOST) {
        (*lease)->cancel();
    }
    CHECK((*lease)->wait_prepared() == E_PENDING);
    controls->release_block(operation);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(operation) == 3U);
    CHECK((*lease)->try_activate() == false);
}

TEST_CASE("dinput.exclusive_native_background_cancellation_beats_late_initial_success",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    controls->block_next(operation);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(operation));
    (*lease)->cancel();
    controls->release_block(operation);

    CHECK((*lease)->wait_prepared() == DIERR_INPUTLOST);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(operation) == 1U);
}

TEST_CASE("dinput.exclusive_native_background_capability_failure_stops_polling",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    controls->set_result(native_operation::get_capabilities, DIERR_UNSUPPORTED);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    CHECK((*lease)->wait_prepared() == DIERR_UNSUPPORTED);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::poll) == 0U);
    CHECK(controls->count(native_operation::get_device_state) == 0U);
}

TEST_CASE("dinput.exclusive_native_background_poll_failure_latches_notacquired",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    controls->set_result(native_operation::poll, DIERR_NOTACQUIRED);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    CHECK((*lease)->wait_prepared() == DIERR_NOTACQUIRED);
    CHECK((*lease)->authority_status() == DIERR_NOTACQUIRED);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::poll) == 1U);
    CHECK(controls->count(native_operation::get_device_state) == 0U);
}

TEST_CASE("dinput.exclusive_native_background_cancellation_interrupts_blocked_poll",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    controls->block_next(native_operation::poll);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::poll));
    (*lease)->cancel();
    CHECK((*lease)->wait_prepared() == DIERR_INPUTLOST);
    CHECK(controls->count(native_operation::get_device_state) == 0U);
    controls->release_block(native_operation::poll);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::get_device_state) == 0U);
}

TEST_CASE("dinput.exclusive_native_background_time_in_poll_cannot_refresh_expired_proof",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE((*lease)->wait_prepared() == DI_OK);
    REQUIRE((*lease)->try_activate());

    controls->block_next(native_operation::poll);
    REQUIRE(controls->wait_for_entry(native_operation::poll));
    clock->advance(250ms);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    controls->release_block(native_operation::poll);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    CHECK(controls->count(native_operation::get_device_state) == 1U);
}

TEST_CASE("dinput.exclusive_native_background_initial_proof_includes_time_in_poll",
    "[integration][dinput][exclusive][native][background][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    controls->block_next(native_operation::poll);

    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE(controls->wait_for_entry(native_operation::poll));
    clock->advance(250ms);
    controls->release_block(native_operation::poll);

    CHECK((*lease)->wait_prepared() == DIERR_INPUTLOST);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    CHECK((*lease)->try_activate() == false);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK(controls->count(native_operation::get_device_state) == 1U);
}

TEST_CASE("dinput.exclusive_native_foreground_does_not_query_background_poll_capabilities",
    "[integration][dinput][exclusive][native][foreground][poll]") {
    native_service_fixture fixture;
    REQUIRE(mark_selected_device_polled(fixture));
    const auto controls = fixture.controls();
    const auto lease = fixture.reserve(fixture.request());
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE((*lease)->wait_prepared() == DI_OK);
    CHECK((*lease)->try_activate());
    CHECK(controls->count(native_operation::get_capabilities) == 0U);
    CHECK(controls->count(native_operation::poll) == 0U);
    (*lease)->cancel();
    CHECK((*lease)->wait_closed() == DI_OK);
}

TEST_CASE("dinput.exclusive_native_background_late_success_cannot_bridge_expired_proof",
    "[integration][dinput][exclusive][native][background]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE((*lease)->wait_prepared() == DI_OK);
    REQUIRE((*lease)->try_activate());

    clock->advance(1ms);
    controls->block_next(operation);
    REQUIRE(controls->wait_for_entry(operation));
    clock->advance(248ms);
    CHECK((*lease)->live());
    clock->advance(1ms);
    bool cancel_before_completion{};
    SECTION("reader observes expiry while probe is blocked") {
        CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
        cancel_before_completion = true;
    }
    SECTION("maintenance observes expiry while probe is blocked") {
        fixture.catalogue.context.access->refresh_eligibility();
        cancel_before_completion = true;
    }
    SECTION("actor observes expiry at probe completion") {
        cancel_before_completion = false;
    }
    CHECK(WaitForSingleObject((*lease)->cancellation_event(), 0U) ==
          (cancel_before_completion ? WAIT_OBJECT_0 : WAIT_TIMEOUT));

    controls->release_block(operation);
    REQUIRE((*lease)->wait_closed() == DI_OK);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    CHECK((*lease)->live() == false);
    CHECK((*lease)->try_activate() == false);
}

TEST_CASE("dinput.exclusive_native_background_pending_refresh_does_not_renew_proof",
    "[integration][dinput][exclusive][native][background]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    const auto controls = fixture.controls();
    constexpr auto operation = native_operation::get_device_state;
    const auto lease = fixture.reserve_background(clock);
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE((*lease)->wait_prepared() == DI_OK);
    REQUIRE((*lease)->try_activate());

    clock->advance(100ms);
    controls->set_result(operation, E_PENDING);
    controls->block_next(operation);
    REQUIRE(controls->wait_for_entry(operation));
    controls->release_block(operation, true);
    REQUIRE(controls->wait_for_entry(operation, 2U));
    CHECK((*lease)->live());
    clock->advance(150ms);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    controls->release_block(operation);
    CHECK((*lease)->wait_closed() == DI_OK);
}

TEST_CASE("dinput.exclusive_native_background_releases_when_the_window_is_destroyed",
    "[integration][dinput][exclusive][native][background][lifecycle]") {
    native_service_fixture fixture;
    const auto clock = std::make_shared<test::focus::manual_clock>();
    auto window = std::make_unique<hidden_window>();
    const auto lease = fixture.reserve_background(clock, window->get());
    REQUIRE(lease.has_value());
    REQUIRE((*lease)->start() == DI_OK);
    REQUIRE((*lease)->wait_prepared() == DI_OK);
    REQUIRE((*lease)->try_activate());

    window.reset();
    CHECK(WaitForSingleObject((*lease)->cancellation_event(), 2'000U) == WAIT_OBJECT_0);
    CHECK((*lease)->authority_status() == DIERR_INPUTLOST);
    (*lease)->cancel();
    CHECK((*lease)->wait_closed() == DI_OK);
    CHECK(selected_count(*fixture.controls(), native_operation::unacquire) == 1U);
}
