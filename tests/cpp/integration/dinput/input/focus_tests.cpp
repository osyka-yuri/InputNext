#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/input_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dinput.h>
#include <inputnext/focus.hpp>

#include <chrono>

using namespace inputnext;
using namespace inputnext::dinput_test_support;
using namespace std::chrono_literals;

TEST_CASE("dinput.foreground_acquisition_is_exact_window_and_sticky_across_a_b_a",
    "[integration][dinput][input][focus]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const hidden_window window_a;
    const hidden_window window_b;
    focus_window_fixture focus_observer;
    focus_observer.set_window(window_a.id(), 2U);
    REQUIRE(focus_observer.wait_for_window(window_a.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, focus_observer.observer());
    auto device = create_device(target, access, parent, 0x71U);
    configure_compact_device(*device.get(), window_a.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);

    compact_state state{};
    REQUIRE(device->Acquire() == DI_OK);
    CHECK(device->Acquire() == S_FALSE);
    REQUIRE(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);

    focus_observer.set_window(window_b.id(), 3U);
    REQUIRE(focus_observer.wait_for_window(window_b.id(), 3U));
    const auto before_loss = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == before_loss);

    focus_observer.set_window(window_a.id(), 4U);
    REQUIRE(focus_observer.wait_for_window(window_a.id(), 4U));
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    REQUIRE(device->Acquire() == DI_OK);
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.foreground_expiry_requires_a_new_explicit_acquisition",
    "[integration][dinput][input][focus][freshness]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const hidden_window window;
    focus_window_fixture focus_observer;
    focus_observer.set_window(window.id(), 2U);
    REQUIRE(focus_observer.wait_for_window(window.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, focus_observer.observer());
    auto device = create_device(target, access, parent, 0x72U);
    configure_compact_device(*device.get(), window.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    REQUIRE(device->Acquire() == DI_OK);

    const auto previous = focus_observer.observer().published();
    focus_observer.source().block_next_observation();
    REQUIRE(focus_observer.wait_until_blocked());
    focus_observer.clock().advance(300ms);

    auto state = sentinel_state(0x5AU);
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(state == sentinel);

    focus_observer.source().release_blocked_observation();
    REQUIRE(focus_observer.wait_for_window(window.id(), previous.window_transition));
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    REQUIRE(device->Acquire() == DI_OK);
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.background_transition_loses_foreground_acquisition_until_reacquired",
    "[integration][dinput][input][focus]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const hidden_window window;
    focus_window_fixture focus_observer;
    focus_observer.set_window(window.id(), 2U);
    REQUIRE(focus_observer.wait_for_window(window.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, focus_observer.observer());
    auto device = create_device(target, access, parent, 0x74U);
    configure_compact_device(*device.get(), window.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    REQUIRE(device->Acquire() == DI_OK);

    focus_observer.set_foreground(false, window.id(), 3U);
    REQUIRE(focus_observer.wait_for_evidence(focus::evidence::background));
    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);

    focus_observer.set_foreground(true, window.id(), 4U);
    REQUIRE(focus_observer.wait_for_window(window.id(), 4U));
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    REQUIRE(device->Acquire() == DI_OK);
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
}

TEST_CASE("dinput.ready_state_reads_do_not_probe_focus_or_native_domains",
    "[integration][dinput][input][cache]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const hidden_window window;
    focus_window_fixture focus_observer;
    focus_observer.set_window(window.id(), 2U);
    REQUIRE(focus_observer.wait_for_window(window.id(), 2U));

    parent_unknown parent;
    const auto access = make_access(owners, focus_observer.observer());
    auto device = create_device(target, access, parent, 0x73U);
    configure_compact_device(*device.get(), window.get(), DISCL_FOREGROUND | DISCL_NONEXCLUSIVE);
    REQUIRE(device->Acquire() == DI_OK);

    focus_observer.source().block_next_observation();
    REQUIRE(focus_observer.wait_until_blocked());
    const auto focus_calls = focus_observer.source().calls();
    const auto current_calls = owners.gameinput.native->current_calls;
    const auto facts_calls = owners.gameinput.native->facts_calls;
    const auto history_factories = owners.scripts[0U]->cursor_factories.load();
    const auto rumble = owners.gameinput.native->rumble_script();
    const auto output_factories = rumble->factory_calls();
    const auto output_writes = rumble->writes();

    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(focus_observer.source().calls() == focus_calls);
    CHECK(owners.gameinput.native->current_calls == current_calls);
    CHECK(owners.gameinput.native->facts_calls == facts_calls);
    CHECK(owners.scripts[0U]->cursor_factories.load() == history_factories);
    CHECK(rumble->factory_calls() == output_factories);
    CHECK(rumble->writes() == output_writes);
    focus_observer.source().release_blocked_observation();
}
