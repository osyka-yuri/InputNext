#include "gameinput/core/publication_source.hpp"
#include "gameinput/history/history_capture_log.hpp"
#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "gameinput/native/native_rumble_endpoint.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "support/gameinput/scripted_native.hpp"
#include "xinput/keystroke_mapping.hpp"
#include "xinput/keystroke_tracker.hpp"
#include "xinput/translation.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/source_lifetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;
using namespace inputnext::xinput::detail;

constexpr button_mask view_mask = static_cast<button_mask>(gamepad_button::view);

[[nodiscard]] runtime_id make_runtime() noexcept {
    runtime_id value{};
    value.bytes[0U] = std::byte{0x72};
    return value;
}

[[nodiscard]] input::program touch_view_program() noexcept {
    input::program_builder builder{};
    static_cast<void>(builder.set(input::source::touch_left,
        input::route{.kind = input::target_kind::button, .button = gamepad_button::view}));
    const auto program = builder.build();
    return program.value_or(input::program::identity());
}

[[nodiscard]] native_input_profile composite_profile() noexcept {
    return native_input_profile{
        .history = input_history_path::composite,
        .reason = input_fallback_reason::none,
        .callback_coherent = true,
        .coordinates = touch_coordinate_source::surface,
        .button_count = 1U,
        .click_button = 0U,
        .surface_index = 0U,
        .surface_width = 100U,
        .surface_height = 80U,
    };
}

[[nodiscard]] input::touch_observation touch_reading(const std::uint64_t timestamp,
    const bool pressed,
    const double x_coordinate = 20.0) noexcept {
    return input::touch_observation{
        .click_status = input::observation_status::observed,
        .click_pressed = pressed,
        .contacts_status = input::observation_status::observed,
        .same_reading = true,
        .contacts_complete = true,
        .surface_width = 100.0,
        .surface_height = 80.0,
        .contacts =
            {input::touch_contact{.id = 1U, .x = x_coordinate, .y = 10.0, .active = pressed},
                input::touch_contact{}},
        .contact_count = pressed ? 1U : 0U,
        .source_timestamp = timestamp,
    };
}

[[nodiscard]] reading_parts callback_reading(const gamepad_state pad,
    const std::uint64_t timestamp,
    const std::uint64_t qpc,
    const bool click_pressed) noexcept {
    return reading_parts{
        .pad = pad,
        .source_timestamp_us = timestamp,
        .observation_qpc = qpc,
        .touch = touch_reading(timestamp, click_pressed),
    };
}

[[nodiscard]] native_history_step native_reading(const gamepad_state pad,
    const input::touch_source touch,
    const std::uint64_t timestamp,
    const std::uint64_t qpc) noexcept {
    return native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = pad,
                .source_timestamp_us = timestamp,
                .observation_qpc = qpc,
            },
        .touch = touch,
    };
}

[[nodiscard]] native_history_seed native_seed(const gamepad_state pad,
    const input::touch_source touch,
    const std::uint64_t timestamp,
    const std::uint64_t qpc) noexcept {
    return native_history_seed{
        .baseline =
            input_update{
                .state = pad,
                .source_timestamp_us = timestamp,
                .observation_qpc = qpc,
            },
        .qpc_frequency = 10'000'000U,
        .touch = touch,
    };
}

struct cursor_plan final {
    native_history_seed seed{};
    std::array<native_history_step, 4U> steps{};
    std::size_t step_count{};
    std::optional<native_failure> start_failure;
};

class planned_native_cursor final : public native_history_cursor {
public:
    explicit planned_native_cursor(cursor_plan& plan) noexcept : plan_(plan) {}

    [[nodiscard]] native_result<native_history_seed> start() noexcept override {
        offset_ = 0U;
        if (plan_.start_failure.has_value()) {
            return std::unexpected(*plan_.start_failure);
        }
        return plan_.seed;
    }

    [[nodiscard]] native_history_step next() noexcept override {
        if (offset_ < plan_.step_count) {
            return plan_.steps[offset_++];
        }
        return native_history_step{
            .kind = native_history_step_kind::caught_up,
            .checkpoint_qpc = plan_.seed.baseline.observation_qpc,
        };
    }

private:
    cursor_plan& plan_;
    std::size_t offset_{};
};

class planned_native final : public native_gameinput {
public:
    std::array<cursor_plan, 4U> plans{};
    std::size_t create_count{};

    [[nodiscard]] native_result<void> initialize(const std::filesystem::path& path) override {
        static_cast<void>(path);
        return {};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) override {
        static_cast<void>(device);
        static_cast<void>(context);
        return native_callback_token{.value = 1U};
    }

    [[nodiscard]] bool unregister_callback(native_callback_token token) noexcept override {
        static_cast<void>(token);
        return true;
    }

    [[nodiscard]] native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) override {
        static_cast<void>(device);
        static_cast<void>(lane);
        return std::unexpected(native_failure{.cause = native_failure_cause::no_current_reading});
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device) override {
        static_cast<void>(device);
        return make_cursor();
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device,
        const native_input_profile& profile,
        const input::touch_snapshot& touch_context) override {
        static_cast<void>(device);
        static_cast<void>(touch_context);
        if (profile.history != input_history_path::composite) {
            return std::unexpected(native_failure{.cause = native_failure_cause::api_unavailable});
        }
        return make_cursor();
    }

    [[nodiscard]] native_result<std::uint64_t> performance_frequency() noexcept override {
        return 10'000'000U;
    }

    [[nodiscard]] native_result<native_device_facts> facts(native_device_handle device) override {
        static_cast<void>(device);
        return native_device_facts{};
    }

    [[nodiscard]] bool connected(native_device_handle device) noexcept override {
        static_cast<void>(device);
        return true;
    }

    [[nodiscard]] native_result<std::unique_ptr<native_rumble_endpoint>> create_rumble_endpoint(
        native_device_handle device) override {
        static_cast<void>(device);
        return std::unexpected(native_failure{.cause = native_failure_cause::resource_exhausted});
    }

    void retain(native_device_handle device) noexcept override { static_cast<void>(device); }

    void release(native_device_handle device) noexcept override { static_cast<void>(device); }

private:
    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> make_cursor() {
        if (create_count >= plans.size()) {
            return std::unexpected(native_failure{.cause = native_failure_cause::api_unavailable});
        }
        auto& plan = plans[create_count++];
        return std::unique_ptr<native_history_cursor>{
            std::make_unique<planned_native_cursor>(plan)};
    }
};

class publication_fixture final {
public:
    publication_fixture()
        : registry(make_runtime()),
          candidate(std::make_unique<publication_source>(publication_epoch_seed{},
              touch_view_program(),
              composite_profile())),
          source(candidate.get()) {
        std::unique_ptr<source_lifetime> owner{std::move(candidate)};
        const auto admitted = registry.admit(owner);
        if (!admitted.has_value()) {
            return;
        }
        connection = admitted.value();
        bind_result = source->bind(connection.producer);
    }

    [[nodiscard]] bool ready() const noexcept { return bind_result == bind_outcome::bound; }

    [[nodiscard]] gamepad_state latest_state() const {
        const auto read = connection.reader.read();
        return read.has_value() ? read->sample.state : gamepad_state{};
    }

    device_registry registry;
    std::unique_ptr<publication_source> candidate;
    publication_source* source{};
    admitted_connection connection{};
    bind_outcome bind_result{bind_outcome::closed};
};

[[nodiscard]] key_mask xinput_keys(gamepad_state state) noexcept {
    XINPUT_GAMEPAD gamepad{};
    translate_state(state, gamepad, false);
    return map_keystroke_keys(gamepad);
}

} // namespace

TEST_CASE("remap.provider_switch_rebases_one_held_key_across_native_capture_native",
    "[integration][remap][history][xinput][provider-switch]") {
    scripted_now = history_time{};
    const publication_fixture fixture;
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);

    REQUIRE(fixture.source->on_callback(callback_reading({}, 10U, 1000U, true)) ==
            publication_outcome::published);
    REQUIRE(fixture.source->on_callback(callback_reading({}, 11U, 1001U, false)) ==
            publication_outcome::published);
    CHECK(fixture.source->status().touch.first_accepted_timestamp == 10U);
    CHECK(fixture.source->status().touch.source == input::touch_source::none);

    planned_native native;
    native.plans[0U].seed = native_seed({}, input::touch_source::none, 100U, 1000U);
    native.plans[0U].steps[0U] =
        native_reading(gamepad_state{.buttons = view_mask}, input::touch_source::none, 101U, 1001U);
    native.plans[0U].steps[1U].kind = native_history_step_kind::history_lost;
    native.plans[0U].step_count = 2U;

    native.plans[1U].start_failure = native_failure{
        .cause = native_failure_cause::no_current_reading,
    };

    native.plans[2U].seed = native_seed({}, input::touch_source::left, 200U, 2000U);

    native.plans[3U].seed = native_seed({}, input::touch_source::left, 300U, 3000U);
    native.plans[3U].steps[0U] =
        native_reading(gamepad_state{.buttons = view_mask}, input::touch_source::none, 301U, 3001U);
    native.plans[3U].steps[1U] = native_reading({}, input::touch_source::none, 302U, 3002U);
    native.plans[3U].step_count = 2U;

    int device{};
    source_history_cursor native_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(native_cursor);
    const auto native_baseline = native_cursor.start(history_time{});
    REQUIRE(native_baseline.has_value());
    CHECK(native_baseline->source == history_source_kind::native_history);
    CHECK((native_baseline->baseline.state.buttons & view_mask) == 0U);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({
                .mask = xinput_keys(native_baseline->baseline.state),
                .qpc = native_baseline->baseline.effective_qpc,
            }) == keystroke_operation_result::applied);

    const auto native_press = native_cursor.next(history_time{});
    REQUIRE(native_press.kind == history_detail::cursor_step_kind::reading);
    CHECK(native_press.origin == history_record_origin::native_pad);
    CHECK((native_press.reading.state.buttons & view_mask) != 0U);
    REQUIRE(tracker.checked_apply_transitions({
                .mask = xinput_keys(native_press.reading.state),
                .qpc = native_press.reading.effective_qpc,
            }) == keystroke_operation_result::applied);
    XINPUT_KEYSTROKE key{};
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYDOWN);

    const auto native_loss = native_cursor.next(history_time{});
    REQUIRE(native_loss.kind == history_detail::cursor_step_kind::history_lost);

    // Callback capture transfers View from the physical button to the touch click in one frame.
    REQUIRE(fixture.source->on_callback(callback_reading({}, 20U, 2000U, true)) ==
            publication_outcome::published);
    CHECK((fixture.latest_state().buttons & view_mask) != 0U);
    CHECK(fixture.source->status().touch.source == input::touch_source::left);

    source_history_cursor fallback_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(fallback_cursor);
    const auto fallback_baseline = fallback_cursor.start(history_time{});
    REQUIRE(fallback_baseline.has_value());
    CHECK(fallback_baseline->source == history_source_kind::callback_capture);
    CHECK((fallback_baseline->baseline.state.buttons & view_mask) != 0U);
    REQUIRE(tracker.rebase({
                .mask = xinput_keys(fallback_baseline->baseline.state),
                .qpc = fallback_baseline->baseline.effective_qpc,
            }) == keystroke_operation_result::applied);
    CHECK_FALSE(tracker.pop(0U, key));

    const auto retry_time = history_time{} + std::chrono::seconds{1};
    CHECK(fallback_cursor.next(history_time{}).kind == history_detail::cursor_step_kind::progress);
    scripted_now = retry_time;
    CHECK(fallback_cursor.next(retry_time).kind == history_detail::cursor_step_kind::progress);
    CHECK(fallback_cursor.next(retry_time).kind == history_detail::cursor_step_kind::history_lost);

    source_history_cursor promoted_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(promoted_cursor);
    const auto promoted_baseline = promoted_cursor.start(retry_time);
    REQUIRE(promoted_baseline.has_value());
    CHECK(promoted_baseline->source == history_source_kind::native_history);
    CHECK((promoted_baseline->baseline.state.buttons & view_mask) != 0U);
    CHECK(native.create_count == 4U);
    REQUIRE(tracker.rebase({
                .mask = xinput_keys(promoted_baseline->baseline.state),
                .qpc = promoted_baseline->baseline.effective_qpc,
            }) == keystroke_operation_result::applied);
    CHECK_FALSE(tracker.pop(0U, key));

    // Native history changes the contributor back to physical View while touch releases.
    const auto physical_handoff = promoted_cursor.next(retry_time);
    REQUIRE(physical_handoff.kind == history_detail::cursor_step_kind::reading);
    CHECK((physical_handoff.reading.state.buttons & view_mask) != 0U);
    REQUIRE(tracker.checked_apply_transitions({
                .mask = xinput_keys(physical_handoff.reading.state),
                .qpc = physical_handoff.reading.effective_qpc,
            }) == keystroke_operation_result::applied);
    CHECK_FALSE(tracker.pop(0U, key));

    const auto released = promoted_cursor.next(retry_time);
    REQUIRE(released.kind == history_detail::cursor_step_kind::reading);
    CHECK((released.reading.state.buttons & view_mask) == 0U);
    REQUIRE(tracker.checked_apply_transitions({
                .mask = xinput_keys(released.reading.state),
                .qpc = released.reading.effective_qpc,
            }) == keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYUP);
    CHECK_FALSE(tracker.pop(0U, key));
    scripted_now = history_time{};
}

TEST_CASE("remap.provider_switch_cancels_a_delivered_key_after_baseline_reset",
    "[integration][remap][history][xinput][provider-switch]") {
    keystroke_tracker tracker;
    const auto view_keys = xinput_keys(gamepad_state{.buttons = view_mask});
    REQUIRE(tracker.silent_seed({.mask = 0U, .qpc = 4000U}) == keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = view_keys, .qpc = 4001U}) ==
            keystroke_operation_result::applied);

    XINPUT_KEYSTROKE key{};
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYDOWN);

    // The old KEYUP is queued but not delivered when recovery discards that history stream.
    REQUIRE(tracker.checked_apply_transitions({.mask = 0U, .qpc = 4002U}) ==
            keystroke_operation_result::applied);
    REQUIRE(tracker.rebase({.mask = 0U, .qpc = 4003U}) == keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYUP);
    CHECK_FALSE(tracker.pop(0U, key));
}

TEST_CASE("remap.bootstrap_fallback_keeps_a_long_touch_hold_with_a_slow_reader",
    "[integration][remap][history][touch][fallback][stress]") {
    constexpr std::uint64_t frame_period_us = 1'000U;
    constexpr std::uint64_t qpc_per_frame = 10'000U;
    constexpr std::size_t hold_frames = 5'000U;
    constexpr std::size_t frames_before_recovery = history_capture_capacity + 64U;
    constexpr std::size_t frames_after_recovery = hold_frames - frames_before_recovery;

    scripted_now = history_time{};
    const publication_fixture fixture;
    REQUIRE(fixture.ready());
    fixture.source->set_callback_coverage(true);
    REQUIRE(
        fixture.source->on_callback(callback_reading({}, frame_period_us, qpc_per_frame, false)) ==
        publication_outcome::published);

    planned_native native;
    for (auto& plan : native.plans) {
        plan.start_failure = native_failure{
            .cause = native_failure_cause::no_current_reading,
            .input_reason = input_fallback_reason::bootstrap_budget,
        };
    }

    int device{};
    source_history_cursor slow_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor fast_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(slow_cursor);
    source_history_cursor_test_access::use_scripted_clock(fast_cursor);
    const auto slow_seed = slow_cursor.start(history_time{});
    const auto fast_seed = fast_cursor.start(history_time{});
    REQUIRE(slow_seed.has_value());
    REQUIRE(fast_seed.has_value());
    CHECK(slow_seed->source == history_source_kind::callback_capture);
    CHECK(fast_seed->source == history_source_kind::callback_capture);
    CHECK(fixture.source->history().diagnostics().last_reason ==
          input_fallback_reason::bootstrap_budget);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({
                .mask = xinput_keys(slow_seed->baseline.state),
                .qpc = slow_seed->baseline.effective_qpc,
            }) == keystroke_operation_result::applied);

    auto frame = std::uint64_t{2U};
    const auto click_timestamp = frame * frame_period_us;
    const auto click_qpc = frame * qpc_per_frame;
    REQUIRE(fixture.source->on_callback(callback_reading({}, click_timestamp, click_qpc, true)) ==
            publication_outcome::published);

    const auto slow_press = slow_cursor.next(history_time{});
    const auto fast_press = fast_cursor.next(history_time{});
    REQUIRE(slow_press.kind == history_detail::cursor_step_kind::reading);
    REQUIRE(fast_press.kind == history_detail::cursor_step_kind::reading);
    CHECK((slow_press.reading.state.buttons & view_mask) != 0U);
    CHECK((fast_press.reading.state.buttons & view_mask) != 0U);
    REQUIRE(tracker.checked_apply_transitions({
                .mask = xinput_keys(slow_press.reading.state),
                .qpc = slow_press.reading.effective_qpc,
            }) == keystroke_operation_result::applied);
    XINPUT_KEYSTROKE key{};
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYDOWN);

    // Model a sustained 1 kHz hold. The native budget result is injected here; the native
    // cursor's real 256-transition limit is exercised by the focused component test.
    for (std::size_t index = 0U; index < frames_before_recovery; ++index) {
        ++frame;
        const auto timestamp = frame * frame_period_us;
        const auto qpc = frame * qpc_per_frame;
        REQUIRE(fixture.source->on_callback(callback_reading({}, timestamp, qpc, true)) ==
                publication_outcome::published);

        const auto fast_reading = fast_cursor.next(history_time{});
        REQUIRE(fast_reading.kind == history_detail::cursor_step_kind::reading);
        CHECK(fast_reading.origin == history_record_origin::callback_pad);
        CHECK((fast_reading.reading.state.buttons & view_mask) != 0U);
        CHECK(fast_reading.reading.source_timestamp_us == timestamp);
    }

    const auto slow_loss = slow_cursor.next(history_time{});
    REQUIRE(slow_loss.kind == history_detail::cursor_step_kind::history_lost);

    source_history_cursor recovered_cursor{native,
        native_device_handle{&device},
        fixture.source->history()};
    source_history_cursor_test_access::use_scripted_clock(recovered_cursor);
    const auto recovered_seed = recovered_cursor.start(history_time{});
    REQUIRE(recovered_seed.has_value());
    CHECK(recovered_seed->source == history_source_kind::callback_capture);
    CHECK((recovered_seed->baseline.state.buttons & view_mask) != 0U);
    REQUIRE(tracker.rebase({
                .mask = xinput_keys(recovered_seed->baseline.state),
                .qpc = recovered_seed->baseline.effective_qpc,
            }) == keystroke_operation_result::applied);
    CHECK_FALSE(tracker.pop(0U, key));

    for (std::size_t index = 0U; index < frames_after_recovery; ++index) {
        ++frame;
        const auto timestamp = frame * frame_period_us;
        const auto qpc = frame * qpc_per_frame;
        REQUIRE(fixture.source->on_callback(callback_reading({}, timestamp, qpc, true)) ==
                publication_outcome::published);

        const auto fast_reading = fast_cursor.next(history_time{});
        const auto recovered_reading = recovered_cursor.next(history_time{});
        REQUIRE(fast_reading.kind == history_detail::cursor_step_kind::reading);
        REQUIRE(recovered_reading.kind == history_detail::cursor_step_kind::reading);
        CHECK((fast_reading.reading.state.buttons & view_mask) != 0U);
        CHECK((recovered_reading.reading.state.buttons & view_mask) != 0U);
        REQUIRE(tracker.checked_apply_transitions({
                    .mask = xinput_keys(recovered_reading.reading.state),
                    .qpc = recovered_reading.reading.effective_qpc,
                }) == keystroke_operation_result::applied);
        CHECK_FALSE(tracker.pop(0U, key));
    }

    ++frame;
    const auto release_timestamp = frame * frame_period_us;
    const auto release_qpc = frame * qpc_per_frame;
    REQUIRE(
        fixture.source->on_callback(callback_reading({}, release_timestamp, release_qpc, false)) ==
        publication_outcome::published);
    const auto fast_release = fast_cursor.next(history_time{});
    const auto recovered_release = recovered_cursor.next(history_time{});
    REQUIRE(fast_release.kind == history_detail::cursor_step_kind::reading);
    REQUIRE(recovered_release.kind == history_detail::cursor_step_kind::reading);
    CHECK((fast_release.reading.state.buttons & view_mask) == 0U);
    CHECK((recovered_release.reading.state.buttons & view_mask) == 0U);
    REQUIRE(tracker.checked_apply_transitions({
                .mask = xinput_keys(recovered_release.reading.state),
                .qpc = recovered_release.reading.effective_qpc,
            }) == keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, key));
    CHECK(key.VirtualKey == 0x5815U);
    CHECK(key.Flags == XINPUT_KEYSTROKE_KEYUP);
    CHECK_FALSE(tracker.pop(0U, key));

    scripted_now = history_time{};
}
