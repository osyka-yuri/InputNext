#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "runtime_test_support.hpp"
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::gameinput_test_support;

[[nodiscard]] reading_parts
touch_reading(bool clicked, std::uint64_t timestamp, std::uint64_t qpc) noexcept {
    return reading_parts{
        .pad = test_pad(0U),
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
        .touch =
            input::touch_observation{
                .click_status = input::observation_status::observed,
                .click_pressed = clicked,
                .contacts_status = input::observation_status::unsupported,
                .source_timestamp = timestamp,
            },
    };
}

TEST_CASE("gameinput.runtime.qualifies_touch_at_admission_and_reconnect",
    "[integration][gameinput][input-profile]") {
    runtime_fixture fixture(input::program::defaults());
    const auto device = handle(fixture.device_memory[0U]);
    fixture.native->set_input_profile(device,
        native_input_profile{
            .history = input_history_path::callback_capture,
            .reason = input_fallback_reason::none,
            .callback_coherent = true,
        });
    fixture.native->set_current(device,
        native_reading_lane::gamepad,
        touch_reading(false, 100U, 1'000U));

    REQUIRE(fixture.start().has_value());
    const auto base = fixture.now;
    fixture.connect(device);
    fixture.runtime->maintain(base);
    REQUIRE(fixture.connection_count() == 1U);
    const auto target = fixture.connection().target;
    CHECK(fixture.native->input_profile_describe_calls(device) == 1U);

    const auto view = static_cast<button_mask>(gamepad_button::view);
    fixture.native->publish(device, touch_reading(true, 150U, 1'500U));
    const auto held = fixture.registry.read(target);
    REQUIRE(held.has_value());
    CHECK(held->sample.state.buttons == view);

    // Metadata changes are observed at the next admission. The existing connection uses
    // its validated descriptor and still reacts to the data supplied by each reading.
    fixture.native->set_input_profile(device, native_input_profile{});
    fixture.runtime->maintain(base + std::chrono::seconds{1});
    fixture.runtime->maintain(base + std::chrono::seconds{10});
    CHECK(fixture.native->input_profile_describe_calls(device) == 1U);
    const auto still_held = fixture.registry.read(target);
    REQUIRE(still_held.has_value());
    CHECK(still_held->sample.state.buttons == view);

    const auto south = static_cast<button_mask>(gamepad_button::south);
    fixture.native->publish(device, pad_parts(test_pad(south), 200U, 2'000U));
    const auto missing_click = fixture.registry.read(target);
    REQUIRE(missing_click.has_value());
    CHECK(missing_click->sample.state.buttons == south);

    fixture.native->publish(device, touch_reading(true, 250U, 2'500U));
    const auto recovered_held = fixture.registry.read(target);
    REQUIRE(recovered_held.has_value());
    CHECK(recovered_held->sample.state.buttons == 0U);

    fixture.native->publish(device, touch_reading(false, 300U, 3'000U));
    fixture.native->publish(device, touch_reading(true, 350U, 3'500U));
    const auto recovered_click = fixture.registry.read(target);
    REQUIRE(recovered_click.has_value());
    CHECK(recovered_click->sample.state.buttons == view);
    CHECK(fixture.native->input_profile_describe_calls(device) == 1U);

    fixture.native->emit(device, native_device_event::disconnected);
    fixture.connect(device);
    const auto reconnected_at = base + std::chrono::seconds{11};
    fixture.runtime->maintain(reconnected_at);
    REQUIRE(fixture.connection_count() == 1U);
    const auto new_target = fixture.connection().target;
    CHECK(new_target != target);
    CHECK_FALSE(fixture.registry.read(target).has_value());
    CHECK(fixture.native->input_profile_describe_calls(device) == 2U);

    auto ordinary = touch_reading(true, 400U, 4'000U);
    ordinary.pad = test_pad(south);
    fixture.native->publish(device, ordinary);
    const auto new_input = fixture.registry.read(new_target);
    REQUIRE(new_input.has_value());
    CHECK(new_input->sample.state.buttons == south);

    fixture.runtime->maintain(reconnected_at + std::chrono::seconds{10});
    CHECK(fixture.native->input_profile_describe_calls(device) == 2U);
    CHECK(fixture.runtime->stop(reconnected_at + std::chrono::seconds{10}).drained);
}

TEST_CASE("gameinput.runtime.skips_touch_qualification_without_touch_routes",
    "[integration][gameinput][input-profile]") {
    runtime_fixture fixture;
    const auto device = handle(fixture.device_memory[0U]);
    REQUIRE(fixture.start().has_value());
    fixture.connect(device);
    fixture.maintain();
    REQUIRE(fixture.connection_count() == 1U);
    fixture.runtime->maintain(fixture.now + std::chrono::seconds{10});
    CHECK(fixture.native->input_profile_describe_calls(device) == 0U);
    CHECK(fixture.runtime->stop(fixture.now + std::chrono::seconds{10}).drained);
}

} // namespace
