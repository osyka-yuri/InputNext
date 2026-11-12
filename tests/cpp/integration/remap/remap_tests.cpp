#include "gameinput/core/publication_source.hpp"
#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "support/gameinput/scripted_native.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;

[[nodiscard]] constexpr button_mask mask(const gamepad_button button) noexcept {
    return static_cast<button_mask>(button);
}

[[nodiscard]] runtime_id make_runtime() {
    runtime_id value{};
    value.bytes[0U] = std::byte{0x71};
    return value;
}

[[nodiscard]] input::touch_observation
touch_reading(bool pressed, float coordinate_x = 20.0F, std::uint64_t contact_id = 1U) {
    input::touch_observation observation{};
    observation.click_status = input::observation_status::observed;
    observation.click_pressed = pressed;
    observation.contacts_status = input::observation_status::observed;
    observation.same_reading = true;
    observation.contacts_complete = true;
    observation.surface_width = 100.0F;
    observation.surface_height = 80.0F;
    observation.contacts[0U] =
        input::touch_contact{.id = contact_id, .x = coordinate_x, .y = 10.0F, .active = true};
    observation.contact_count = 1U;
    return observation;
}

[[nodiscard]] input::touch_observation missing_click() {
    auto observation = touch_reading(false);
    observation.click_status = input::observation_status::unavailable;
    return observation;
}

[[nodiscard]] reading_parts reading(gamepad_state pad,
    input::touch_observation touch,
    std::uint64_t timestamp,
    std::uint64_t qpc) {
    touch.source_timestamp = timestamp;
    return reading_parts{
        .pad = pad,
        .guide = std::nullopt,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
        .touch = touch,
    };
}

class publication_fixture final {
public:
    publication_fixture(input::program mappings, native_input_profile profile)
        : registry(make_runtime()) {
        auto candidate =
            std::make_unique<publication_source>(publication_epoch_seed{}, mappings, profile);
        source = candidate.get();
        std::unique_ptr<source_lifetime> owner{std::move(candidate)};
        const auto admitted = registry.admit(owner);
        if (!admitted.has_value()) {
            return;
        }
        connection = admitted.value();
        bind_result = source->bind(connection.producer);
    }

    [[nodiscard]] bool ready() const noexcept { return bind_result == bind_outcome::bound; }

    device_registry registry;
    publication_source* source{};
    admitted_connection connection{};
    bind_outcome bind_result{bind_outcome::closed};
};

[[nodiscard]] native_input_profile callback_profile() noexcept {
    return native_input_profile{
        .history = input_history_path::callback_capture,
        .reason = input_fallback_reason::none,
        .callback_coherent = true,
        .coordinates = touch_coordinate_source::surface,
    };
}

[[nodiscard]] input::program remap_with_touch() {
    input::program_builder builder{};
    static_cast<void>(builder.set(input::source::face_bottom,
        input::route{.kind = input::target_kind::button, .button = gamepad_button::east}));
    static_cast<void>(builder.set(input::source::face_right,
        input::route{.kind = input::target_kind::button, .button = gamepad_button::south}));
    static_cast<void>(builder.set(input::source::select, input::route{}));
    static_cast<void>(builder.set(input::source::touch_left,
        input::route{.kind = input::target_kind::button, .button = gamepad_button::view}));
    return builder.build().value_or(input::program::identity());
}

[[nodiscard]] gamepad_state latest_state(const publication_fixture& fixture) {
    const auto result = fixture.connection.reader.read();
    return result.has_value() ? result->sample.state : gamepad_state{};
}

} // namespace

TEST_CASE("remap.callback_capture_projects_one_atomic_frame_without_cascading",
    "[integration][remap][publication][history]") {
    const publication_fixture fixture{remap_with_touch(), callback_profile()};
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);

    const auto raw_pad =
        gamepad_state{.buttons = mask(gamepad_button::south) | mask(gamepad_button::view)};
    const auto release = touch_reading(false);
    const auto seed = fixture.source->before_seed();
    REQUIRE(fixture.source->on_seed(seed, reading(raw_pad, release, 100U, 1000U)) ==
            publication_outcome::published);

    const auto initial = latest_state(fixture);
    CHECK((initial.buttons & mask(gamepad_button::east)) != 0U);
    CHECK((initial.buttons & mask(gamepad_button::south)) == 0U);
    CHECK((initial.buttons & mask(gamepad_button::view)) == 0U);

    scripted_native native;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    const auto baseline = cursor.start(history_time{});
    REQUIRE(baseline.has_value());
    const auto baseline_value = baseline.value_or(history_detail::cursor_seed{});
    CHECK(baseline_value.source == history_source_kind::callback_capture);
    CHECK((baseline_value.baseline.state.buttons & mask(gamepad_button::east)) != 0U);
    CHECK((baseline_value.baseline.state.buttons & mask(gamepad_button::view)) == 0U);

    const auto press = touch_reading(true, 20.0F);
    REQUIRE(fixture.source->on_callback(reading(raw_pad, press, 101U, 1001U)) ==
            publication_outcome::published);
    const auto pressed = latest_state(fixture);
    CHECK((pressed.buttons & mask(gamepad_button::east)) != 0U);
    CHECK((pressed.buttons & mask(gamepad_button::south)) == 0U);
    // The disabled physical View route does not block the independent touch source.
    CHECK((pressed.buttons & mask(gamepad_button::view)) != 0U);

    const auto historical_press = cursor.next(history_time{});
    REQUIRE(historical_press.kind == history_detail::cursor_step_kind::reading);
    CHECK((historical_press.reading.state.buttons & mask(gamepad_button::east)) != 0U);
    CHECK((historical_press.reading.state.buttons & mask(gamepad_button::south)) == 0U);
    CHECK((historical_press.reading.state.buttons & mask(gamepad_button::view)) != 0U);
    CHECK(native.create_count == 0U);

    REQUIRE(fixture.source->on_callback(reading(raw_pad, touch_reading(false), 102U, 1002U)) ==
            publication_outcome::published);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) == 0U);
    const auto historical_release = cursor.next(history_time{});
    REQUIRE(historical_release.kind == history_detail::cursor_step_kind::reading);
    CHECK((historical_release.reading.state.buttons & mask(gamepad_button::view)) == 0U);
    const auto delayed = cursor.next(history_time{} + std::chrono::seconds{2});
    CHECK(delayed.kind == history_detail::cursor_step_kind::progress);
    CHECK(native.create_count == 0U);
}

TEST_CASE("remap.touch_source_transfer_latches_and_recovers_after_click_loss",
    "[integration][remap][publication][history]") {
    const publication_fixture fixture{input::program::defaults(), callback_profile()};
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);

    const auto physical_view = gamepad_state{.buttons = mask(gamepad_button::view)};
    REQUIRE(fixture.source->on_seed(fixture.source->before_seed(),
                reading(physical_view, touch_reading(false), 200U, 2000U)) ==
            publication_outcome::published);

    scripted_native native;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    const auto baseline = cursor.start(history_time{});
    REQUIRE(baseline.has_value());
    const auto baseline_value = baseline.value_or(history_detail::cursor_seed{});
    CHECK((baseline_value.baseline.state.buttons & mask(gamepad_button::view)) != 0U);

    const auto no_view = gamepad_state{};
    REQUIRE(
        fixture.source->on_callback(reading(no_view, touch_reading(true, 15.0F), 201U, 2001U)) ==
        publication_outcome::published);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
    const auto transfer = cursor.next(history_time{});
    REQUIRE(transfer.kind == history_detail::cursor_step_kind::reading);
    // Releasing physical View and pressing the left click in the same frame preserves the target.
    CHECK((transfer.reading.state.buttons & mask(gamepad_button::view)) != 0U);

    REQUIRE(
        fixture.source->on_callback(reading(no_view, touch_reading(true, 90.0F), 202U, 2002U)) ==
        publication_outcome::published);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
    const auto moved_contact = cursor.next(history_time{});
    REQUIRE(moved_contact.kind == history_detail::cursor_step_kind::reading);
    CHECK((moved_contact.reading.state.buttons & mask(gamepad_button::view)) != 0U);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::menu)) == 0U);

    // Losing click data invalidates the touch contribution but leaves the physical contributor.
    REQUIRE(fixture.source->on_callback(reading(physical_view, missing_click(), 203U, 2003U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.awaiting_release);
    CHECK(fixture.source->status().touch.source == input::touch_source::none);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
    CHECK(cursor.next(history_time{}).kind == history_detail::cursor_step_kind::history_lost);
    CHECK(native.create_count == 0U);

    source_history_cursor after_loss{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(after_loss);
    const auto loss_baseline = after_loss.start(history_time{});
    REQUIRE(loss_baseline.has_value());
    const auto loss_baseline_value = loss_baseline.value_or(history_detail::cursor_seed{});
    CHECK((loss_baseline_value.baseline.state.buttons & mask(gamepad_button::view)) != 0U);

    // A still-held click after recovery is suppressed until a real release is observed.
    REQUIRE(
        fixture.source->on_callback(reading(no_view, touch_reading(true, 15.0F), 204U, 2004U)) ==
        publication_outcome::published);
    CHECK(fixture.source->status().touch.awaiting_release);
    CHECK(fixture.source->status().touch.source == input::touch_source::none);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) == 0U);
    const auto recovered_held = after_loss.next(history_time{});
    REQUIRE(recovered_held.kind == history_detail::cursor_step_kind::reading);
    CHECK((recovered_held.reading.state.buttons & mask(gamepad_button::view)) == 0U);

    REQUIRE(fixture.source->on_callback(reading(no_view, touch_reading(false), 205U, 2005U)) ==
            publication_outcome::published);
    REQUIRE(
        fixture.source->on_callback(reading(no_view, touch_reading(true, 15.0F), 206U, 2006U)) ==
        publication_outcome::published);
    CHECK(fixture.source->status().touch.source == input::touch_source::left);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
}

TEST_CASE("remap.unqualified_profile_ignores_touch_but_keeps_ordinary_remapping",
    "[integration][remap][native-history][qualification]") {
    const publication_fixture fixture{remap_with_touch(), native_input_profile{}};
    REQUIRE(fixture.ready());

    const auto south = gamepad_state{.buttons = mask(gamepad_button::south)};
    REQUIRE(fixture.source->on_callback(reading(south, touch_reading(true, 15.0F), 300U, 3000U)) ==
            publication_outcome::published);
    const auto latest = latest_state(fixture);
    CHECK((latest.buttons & mask(gamepad_button::east)) != 0U);
    CHECK((latest.buttons & mask(gamepad_button::south)) == 0U);
    CHECK((latest.buttons & mask(gamepad_button::view)) == 0U);
    CHECK(
        fixture.source->history().snapshot().input_profile.history == input_history_path::gamepad);
    CHECK(fixture.source->history().snapshot().touch.source == input::touch_source::none);

    scripted_native native;
    native.history.seed.baseline = input_update{
        .state = south,
        .source_timestamp_us = 300U,
        .observation_qpc = 3000U,
    };
    native.history.seed.qpc_frequency = 10'000'000U;
    native.history.steps[0U] = reading_step(mask(gamepad_button::east), 301U, 3001U);
    native.history.step_count = 1U;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    const auto baseline = cursor.start(history_time{});
    REQUIRE(baseline.has_value());
    const auto baseline_value = baseline.value_or(history_detail::cursor_seed{});
    CHECK(baseline_value.source == history_source_kind::native_history);
    CHECK((baseline_value.baseline.state.buttons & mask(gamepad_button::east)) != 0U);
    CHECK(native.create_count == 1U);

    const auto historical = cursor.next(history_time{});
    REQUIRE(historical.kind == history_detail::cursor_step_kind::reading);
    CHECK((historical.reading.state.buttons & mask(gamepad_button::south)) != 0U);
    CHECK((historical.reading.state.buttons & mask(gamepad_button::east)) == 0U);
}

TEST_CASE("remap.initially_held_click_uses_latched_fallback_until_release",
    "[integration][remap][publication]") {
    const publication_fixture fixture{input::program::defaults(), callback_profile()};
    REQUIRE(fixture.ready());

    const auto pad = gamepad_state{};
    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(true, 20.0F), 400U, 4000U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.source == input::touch_source::fallback);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);

    fixture.source->set_callback_coverage(true);
    scripted_native native;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    const auto baseline = cursor.start(history_time{});
    REQUIRE(baseline.has_value());
    const auto baseline_value = baseline.value_or(history_detail::cursor_seed{});
    CHECK((baseline_value.baseline.state.buttons & mask(gamepad_button::view)) != 0U);
    CHECK(baseline_value.source == history_source_kind::callback_capture);

    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(true, 90.0F), 401U, 4001U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.source == input::touch_source::fallback);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::menu)) == 0U);
    const auto moved = cursor.next(history_time{});
    REQUIRE(moved.kind == history_detail::cursor_step_kind::reading);
    CHECK((moved.reading.state.buttons & mask(gamepad_button::view)) != 0U);
    CHECK((moved.reading.state.buttons & mask(gamepad_button::menu)) == 0U);

    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(false), 402U, 4002U)) ==
            publication_outcome::published);
    const auto released = cursor.next(history_time{});
    REQUIRE(released.kind == history_detail::cursor_step_kind::reading);
    CHECK((released.reading.state.buttons & mask(gamepad_button::view)) == 0U);
    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(true, 90.0F), 403U, 4003U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.source == input::touch_source::right);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::menu)) != 0U);
    const auto right_click = cursor.next(history_time{});
    REQUIRE(right_click.kind == history_detail::cursor_step_kind::reading);
    CHECK((right_click.reading.state.buttons & mask(gamepad_button::menu)) != 0U);
    CHECK(native.create_count == 0U);
}

TEST_CASE("remap.stale_seed_cannot_restore_touch_after_coverage_epoch_change",
    "[integration][remap][publication][epochs]") {
    const publication_fixture fixture{input::program::defaults(), callback_profile()};
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);

    const auto pad = gamepad_state{.buttons = mask(gamepad_button::south)};
    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(false), 500U, 5000U)) ==
            publication_outcome::published);
    const auto stale_ticket = fixture.source->before_seed();

    fixture.source->set_callback_coverage(false);
    fixture.source->set_callback_coverage(true);
    REQUIRE(fixture.source->on_seed(stale_ticket,
                reading(pad, touch_reading(false), 501U, 5001U)) == publication_outcome::published);
    CHECK(fixture.source->status().touch.awaiting_release);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::east)) == 0U);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) == 0U);

    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(true, 15.0F), 502U, 5002U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.awaiting_release);
    CHECK(fixture.source->status().touch.source == input::touch_source::none);
    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(false), 503U, 5003U)) ==
            publication_outcome::published);
    REQUIRE(fixture.source->on_callback(reading(pad, touch_reading(true, 15.0F), 504U, 5004U)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.source == input::touch_source::left);
    CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
}

TEST_CASE("remap.diagnostics_separate_qualified_path_from_selected_fallback_and_count_reasons",
    "[integration][remap][diagnostics][history]") {
    auto profile = callback_profile();
    profile.history = input_history_path::composite;
    profile.runtime_version = {4U, 0U, 306U, 0U};
    profile.transport = physical_transport::usb;
    const publication_fixture fixture{input::program::defaults(), profile};
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);

    const auto pad = gamepad_state{.buttons = mask(gamepad_button::south)};
    REQUIRE(fixture.source->on_seed(fixture.source->before_seed(),
                reading(pad, touch_reading(false), 600U, 6000U)) == publication_outcome::published);

    scripted_native native;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    const auto baseline = cursor.start(history_time{});
    REQUIRE(baseline.has_value());
    CHECK(baseline->source == history_source_kind::callback_capture);
    CHECK(native.create_count == 0U);

    auto diagnostics = fixture.source->history().diagnostics();
    CHECK(diagnostics.configured_path == input_history_path::composite);
    CHECK(diagnostics.last_selected_history == input_history_path::callback_capture);
    CHECK(diagnostics.last_reason == input_fallback_reason::incoherent_reading);
    CHECK(diagnostics.counts[static_cast<std::size_t>(input_fallback_reason::incoherent_reading)] ==
          1U);
    const std::array<std::uint16_t, 4U> expected_runtime_version{4U, 0U, 306U, 0U};
    CHECK(diagnostics.profile.runtime_version == expected_runtime_version);
    CHECK(diagnostics.profile.transport == physical_transport::usb);

    fixture.source->history().record_fallback(input_fallback_reason::incomplete_replay);
    fixture.source->history().record_fallback(input_fallback_reason::incomplete_replay);
    diagnostics = fixture.source->history().diagnostics();
    CHECK(diagnostics.last_reason == input_fallback_reason::incomplete_replay);
    CHECK(diagnostics.counts[static_cast<std::size_t>(input_fallback_reason::incomplete_replay)] ==
          2U);
}

TEST_CASE("remap.independent_guide_observations_preserve_held_touch",
    "[integration][remap][publication][history]") {
    const publication_fixture fixture{input::program::defaults(), callback_profile()};
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);
    REQUIRE(fixture.source->on_callback(reading({}, touch_reading(false), 600U, 6000U)) ==
            publication_outcome::published);
    REQUIRE(fixture.source->on_callback(reading({}, touch_reading(true), 601U, 6001U)) ==
            publication_outcome::published);
    const auto epoch = fixture.source->status().touch.epoch;

    scripted_native native;
    int device{};
    source_history_cursor cursor{native, native_device_handle{&device}, fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(cursor);
    REQUIRE(cursor.start(history_time{}).has_value());

    const input::touch_observation independent{
        .click_status = input::observation_status::not_applicable,
        .contacts_status = input::observation_status::not_applicable,
    };
    for (const bool guide_pressed : {true, false}) {
        const reading_parts guide_only{
            .guide = guide_pressed,
            .source_timestamp_us = guide_pressed ? 602U : 603U,
            .observation_qpc = guide_pressed ? 6002U : 6003U,
            .touch = independent,
        };
        REQUIRE(fixture.source->on_callback(guide_only) == publication_outcome::published);
        CHECK(fixture.source->status().touch.epoch == epoch);
        CHECK(fixture.source->status().touch.source == input::touch_source::left);
        CHECK(!fixture.source->status().touch.awaiting_release);
        CHECK((latest_state(fixture).buttons & mask(gamepad_button::view)) != 0U);
        CHECK(
            ((latest_state(fixture).buttons & mask(gamepad_button::guide)) != 0U) == guide_pressed);
        const auto step = cursor.next(history_time{});
        REQUIRE(step.kind == history_detail::cursor_step_kind::reading);
        CHECK((step.reading.state.buttons & mask(gamepad_button::view)) != 0U);
        CHECK(((step.reading.state.buttons & mask(gamepad_button::guide)) != 0U) == guide_pressed);
    }
}
