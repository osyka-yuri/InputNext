#pragma once

#include "gameinput/history/history_capture.hpp"
#include "gameinput/history/source_history_cursor.hpp"
#include "gameinput/input_profile.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "input/program.hpp"
#include "support/gameinput/scripted_native.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace inputnext::gameinput_detail {

inline constexpr auto guide_mask = static_cast<button_mask>(gamepad_button::guide);

class gated_thread final {
public:
    template <typename Function>
    gated_thread(test_gate& gate, Function&& function)
        : gate_(gate),
          thread_(std::forward<Function>(function)) {}

    ~gated_thread() {
        gate_.release();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    gated_thread(const gated_thread&) = delete;
    gated_thread& operator=(const gated_thread&) = delete;
    gated_thread(gated_thread&&) = delete;
    gated_thread& operator=(gated_thread&&) = delete;

    [[nodiscard]] bool wait_until_entered_for(std::chrono::milliseconds timeout) noexcept {
        return gate_.wait_until_entered_for(timeout);
    }

    void finish() noexcept {
        gate_.release();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    test_gate& gate_;
    std::thread thread_;
};

class cursor_fixture final {
public:
    cursor_fixture() : cursor(native, native_device_handle{&device}, *capture) {
        source_history_cursor_test_access::use_scripted_clock(cursor);
    }

    void qualify_guide(bool pressed = false) const {
        capture->set_callback_coverage(true);
        capture->on_seed(reading_parts{
            .guide = pressed,
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
        });
    }

    int device{};
    std::unique_ptr<history_capture> capture{std::make_unique<history_capture>()};
    scripted_native native;
    source_history_cursor cursor;
};

[[nodiscard]] inline input::program touch_view_program() noexcept {
    input::program_builder builder{};
    static_cast<void>(builder.set(input::source::touch_left,
        input::route{.kind = input::target_kind::button, .button = gamepad_button::view}));
    const auto program = builder.build();
    return program.value_or(input::program::identity());
}

[[nodiscard]] inline native_input_profile touch_profile(input_history_path path) noexcept {
    return native_input_profile{
        .history = path,
        .reason = input_fallback_reason::none,
        .callback_coherent = true,
        .coordinates = touch_coordinate_source::none,
    };
}

class touch_cursor_fixture final {
public:
    explicit touch_cursor_fixture(input_history_path path)
        : capture(std::make_unique<history_capture>(touch_view_program(), touch_profile(path))),
          cursor(native, native_device_handle{&device}, *capture) {
        native.allow_composite_history = path == input_history_path::composite;
        source_history_cursor_test_access::use_scripted_clock(cursor);
        capture->set_callback_coverage(true);
        capture->on_seed(reading_parts{
            .pad = gamepad_state{},
            .guide = false,
            .source_timestamp_us = 100U,
            .observation_qpc = 100U,
            .resolved_touch =
                input::touch_snapshot{
                    .source = input::touch_source::left,
                    .click_known_pressed = true,
                    .epoch = 1U,
                    .first_accepted_timestamp = 100U,
                },
        });
    }

    void revoke_touch() const noexcept {
        capture->invalidate_touch(
            input::touch_snapshot{
                .source = input::touch_source::none,
                .awaiting_release = true,
                .epoch = 2U,
                .reason = input::touch_loss_reason::missing_click,
            },
            input_fallback_reason::epoch_changed,
            true);
    }

    int device{};
    std::unique_ptr<history_capture> capture;
    scripted_native native;
    source_history_cursor cursor;
};

} // namespace inputnext::gameinput_detail
