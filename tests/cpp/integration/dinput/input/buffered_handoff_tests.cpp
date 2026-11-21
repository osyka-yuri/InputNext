#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "dinput/buffered/event_registration.hpp"
#include "dinput/buffered/service.hpp"
#include "dinput/buffered/subscription.hpp"
#include "dinput/format.hpp"
#include "dinput/input_access.hpp"
#include "history/reader_observation.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>

#include <array>
#include <chrono>
#include <memory>
#include <utility>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

TEST_CASE("dinput.prepared_notification_is_dispatched_after_activation_with_a_blocked_lane",
    "[integration][dinput][buffered][notification][acquisition]") {
    core_test_support::fixture owners;
    const auto target = owners.connect();
    prepare_buffered_history(owners);
    const auto access = make_access(owners, *owners.focus_observer);
    const auto call = access->enter();
    REQUIRE(static_cast<bool>(call));

    const std::unique_ptr<void, decltype(&CloseHandle)> event{
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
        &CloseHandle};
    REQUIRE(event != nullptr);
    const auto notification = dinput::event_registration::duplicate(event.get());
    REQUIRE(notification.has_value());

    dinput::buffer_configuration configuration{.target = target,
        .requested_size = 8U,
        .notification = notification.value()};
    std::array<DIOBJECTDATAFORMAT, 22U> descriptions{};
    std::array<GUID, 22U> guids{};
    const auto format = compact_format(descriptions, guids);
    REQUIRE(dinput::compile_format(format, configuration.format, configuration.model) == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    const auto subscription = call.buffers().subscribe(std::move(configuration));
    core_test_support::history_call_release_guard release{script};
    REQUIRE(subscription.has_value());
    REQUIRE(script->wait_until_blocked(2s));

    // Drive the prepared handoff directly while the external history dependency is stopped.
    const history_domain domain{.target = target, .stream = 1U};
    subscription.value()->prepare(
        history_detail::reader_boundary{.domain = domain,
            .sequence = 1U,
            .checkpoint_qpc = 100U,
            .clock = {.timestamp_us = buffered_cut_timestamp, .system_time_ms = 100U}},
        history_sample{.state = {}, .source_timestamp_us = 100U, .guide_known = true});
    history_detail::observed_read read{};
    read.batch.domain = domain;
    read.batch.count = 1U;
    read.batch.records.front() =
        history_record{.sample = {.state = {.buttons = buffered_button},
                           .source_timestamp_us = buffered_cut_timestamp + 1U,
                           .effective_qpc = 101U,
                           .guide_known = true},
            .sequence = 2U,
            .origin = history_record_origin::native_pad};
    std::array<DWORD, history_batch_capacity> sequences{};
    sequences.front() = 17U;
    subscription.value()->accept(read, sequences);
    CHECK(WaitForSingleObject(event.get(), 0U) == WAIT_TIMEOUT);
    REQUIRE(subscription.value()->activate());

    // A cache-only sweep can complete notification while native history is still blocked.
    call.buffers().refresh_eligibility();
    CHECK(WaitForSingleObject(event.get(), 2'000U) == WAIT_OBJECT_0);
    std::array<DIDEVICEOBJECTDATA, 8U> events{};
    DWORD count{};
    CHECK(subscription.value()->read(sizeof(DIDEVICEOBJECTDATA), events.data(), 8U, count, false) ==
          DI_OK);
    REQUIRE(count == 1U);
    CHECK(events.front().dwData == 0x80U);
    subscription.value()->cancel();
    subscription.value()->synchronize_signals();
    release.release();
}

TEST_CASE("dinput.failed_reacquire_retires_prior_notification_before_unacquire",
    "[integration][dinput][buffered][notification][acquisition][capacity]") {
    core_test_support::fixture owners;
    const auto target = owners.connect();
    prepare_buffered_history(owners);
    focus_window_fixture foreground;
    const hidden_window window;
    foreground.set_window(window.id(), 2U);
    REQUIRE(foreground.wait_for_window(window.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, foreground.observer());
    auto device = create_device(target, access, parent, 0x81U);
    configure_compact_device(*device.get(), window.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    const std::unique_ptr<void, decltype(&CloseHandle)> event{
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
        &CloseHandle};
    REQUIRE(event != nullptr);
    REQUIRE(device->SetEventNotification(event.get()) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    const auto script = owners.scripts[0U];
    script->block_next();
    core_test_support::history_call_release_guard release{script};
    REQUIRE(script->wait_until_blocked(2s));
    foreground.set_foreground(false, window.id(), 3U);
    REQUIRE(foreground.wait_for_evidence(focus::evidence::background));
    compact_state state{};
    REQUIRE(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    foreground.set_window(window.id(), 4U);
    REQUIRE(foreground.wait_for_window(window.id(), 4U));

    // Keep the old owner resident in the blocked lane and exhaust the remaining slots.
    const auto call = access->enter();
    REQUIRE(static_cast<bool>(call));
    std::array<std::shared_ptr<dinput::buffered_subscription>, 63U> pending{};
    for (auto& subscription : pending) {
        const auto subscribed = call.buffers().subscribe({.target = target});
        REQUIRE(subscribed.has_value());
        subscription = subscribed.value();
    }
    REQUIRE(device->Acquire() == DIERR_OUTOFMEMORY);
    REQUIRE(device->Unacquire() == DI_OK);
    REQUIRE(ResetEvent(event.get()) != FALSE);

    // Retained history snapshots must not deliver the previous acquisition's loss later.
    access->refresh_eligibility();
    CHECK(WaitForSingleObject(event.get(), 0U) == WAIT_TIMEOUT);
    for (const auto& subscription : pending) {
        subscription->cancel();
    }
    release.release();
}
