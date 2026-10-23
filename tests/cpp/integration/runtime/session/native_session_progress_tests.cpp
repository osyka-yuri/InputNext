#include "native_session_test_support.hpp"

#include "cpp/integration/gameinput/runtime/runtime_history_test_support.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>
#include <inputnext/output.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <utility>

namespace inputnext::runtime_session_test_support {
namespace {

constexpr button_mask south_button = static_cast<button_mask>(gamepad_button::south);

class history_release_guard final {
public:
    explicit history_release_guard(std::shared_ptr<native_history_script> history) noexcept
        : history_(std::move(history)) {}

    ~history_release_guard() { release(); }

    history_release_guard(const history_release_guard&) = delete;
    history_release_guard& operator=(const history_release_guard&) = delete;
    history_release_guard(history_release_guard&&) = delete;
    history_release_guard& operator=(history_release_guard&&) = delete;

    void release() noexcept {
        if (history_ != nullptr) {
            history_->release_blocked_call();
            history_.reset();
        }
    }

private:
    std::shared_ptr<native_history_script> history_;
};

} // namespace

TEST_CASE(
    "runtime.session.activates_only_after_publication_and_autonomously_maintains_input_events",
    "[integration][runtime][session][progress]") {
    session_fixture fixture;
    auto history = std::make_shared<native_history_script>();
    fixture.configure_device(history, gamepad_state{});

    REQUIRE(fixture.start().has_value());
    CHECK(fixture.native->fake->active_registrations(fake_gameinput::registration_kind::reading) ==
          0U);
    CHECK_FALSE(fixture.session->presentation().positions()[0U].has_value());

    fixture.session->activate();
    REQUIRE(fixture.wait_for_presentation());
    REQUIRE(fixture.wait_for_output_endpoint());
    REQUIRE(wait_until([&] {
        return fixture.native->fake->active_registrations(
                   fake_gameinput::registration_kind::reading) != 0U;
    }));
    REQUIRE(wait_until([&fixture] {
        XINPUT_STATE state{};
        return fixture.read_state(state) == ERROR_SUCCESS && state.Gamepad.wButtons == 0U &&
               state.Gamepad.bLeftTrigger == 0U && state.Gamepad.bRightTrigger == 0U &&
               state.Gamepad.sThumbLX == 0 && state.Gamepad.sThumbLY == 0 &&
               state.Gamepad.sThumbRX == 0 && state.Gamepad.sThumbRY == 0;
    }));
    CHECK(history->qpc_frequency == current_qpc_frequency());
    CHECK(wait_until(
        [&] { return history->cursor_factories.load(std::memory_order_relaxed) != 0U; }));

    history->block_next();
    history_release_guard release_history{history};
    // The first native next follows the seed-only baseline batch, so this barrier ensures the
    // neutral baseline reached XInput before the ordered button history is queued.
    REQUIRE(history->wait_until_blocked(wait_limit));

    const auto key_down_qpc = current_qpc();
    history->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = gamepad_state{.buttons = south_button},
                .source_timestamp_us = 200U,
                .observation_qpc = key_down_qpc,
            },
    });
    std::uint64_t key_up_qpc{};
    REQUIRE(wait_until([&] {
        key_up_qpc = current_qpc();
        return key_up_qpc > key_down_qpc;
    }));
    history->enqueue(native_history_step{
        .kind = native_history_step_kind::reading,
        .reading =
            input_update{
                .state = gamepad_state{},
                .source_timestamp_us = 300U,
                .observation_qpc = key_up_qpc,
            },
    });
    history->enqueue(native_history_step{
        .kind = native_history_step_kind::caught_up,
        .checkpoint_qpc = key_up_qpc,
    });
    release_history.release();

    const auto read_key = [&fixture](XINPUT_KEYSTROKE& key) {
        DWORD key_result = ERROR_EMPTY;
        const auto key_ready = wait_until([&] {
            key_result =
                fixture.session->presentation().get_keystroke(xinput::flavor::v1_4, 0U, 0U, &key);
            return key_result == ERROR_SUCCESS;
        });
        CHECK(key_ready);
        return key_result;
    };

    XINPUT_KEYSTROKE key_down{};
    REQUIRE(read_key(key_down) == ERROR_SUCCESS);
    CHECK(key_down.VirtualKey == 0x5800U);
    CHECK((key_down.Flags & XINPUT_KEYSTROKE_KEYDOWN) != 0U);

    XINPUT_KEYSTROKE key_up{};
    REQUIRE(read_key(key_up) == ERROR_SUCCESS);
    CHECK(key_up.VirtualKey == key_down.VirtualKey);
    CHECK((key_up.Flags & XINPUT_KEYSTROKE_KEYDOWN) == 0U);
}

TEST_CASE("runtime.session.blocked_native_maintenance_leaves_ready_input_and_output_workers_live",
    "[integration][runtime][session][progress][concurrency]") {
    session_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    fixture.configure_device(history);
    REQUIRE(fixture.start().has_value());
    fixture.session->activate();
    REQUIRE(fixture.wait_for_presentation());
    REQUIRE(fixture.wait_for_output_endpoint());
    REQUIRE(fixture.wait_for_button(XINPUT_GAMEPAD_A));

    fixture.block_next_facts();
    fixture.native->fake->emit(fixture.device, native_device_event::haptic_info_updated);
    REQUIRE(fixture.native->fake->wait_for_blocked_facts());

    std::future<std::pair<DWORD, XINPUT_STATE>> state_read;
    std::future<DWORD> output_write;

    struct blocked_work_release_guard final {
        session_fixture& fixture;

        explicit blocked_work_release_guard(session_fixture& owner) noexcept : fixture(owner) {}

        ~blocked_work_release_guard() { fixture.release_blocked_work(); }

        blocked_work_release_guard(const blocked_work_release_guard&) = delete;
        blocked_work_release_guard& operator=(const blocked_work_release_guard&) = delete;
        blocked_work_release_guard(blocked_work_release_guard&&) = delete;
        blocked_work_release_guard& operator=(blocked_work_release_guard&&) = delete;
    };

    const blocked_work_release_guard release_blocked_work{fixture};

    const auto rumble_writes_before = fixture.rumble_controls->writes();
    const output::rumble_value expected_rumble{
        .low_frequency = 31'000.0F / 65'535.0F,
        .high_frequency = 27'000.0F / 65'535.0F,
    };
    state_read = std::async(std::launch::async, [&fixture] {
        XINPUT_STATE state{};
        return std::pair{fixture.read_state(state), state};
    });
    output_write = std::async(std::launch::async, [&fixture] {
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 31'000U, .wRightMotorSpeed = 27'000U};
        return fixture.session->presentation().set_state(xinput::flavor::v1_4, 0U, &vibration);
    });

    const auto read_ready = state_read.wait_for(wait_limit) == std::future_status::ready;
    const auto write_ready = output_write.wait_for(wait_limit) == std::future_status::ready;
    CHECK(read_ready);
    CHECK(write_ready);
    const auto rumble_written = wait_until([&] {
        return fixture.rumble_controls->writes() > rumble_writes_before &&
               fixture.rumble_controls->last_value() == expected_rumble;
    });
    CHECK(rumble_written);

    fixture.release_facts();
    if (read_ready) {
        const auto [result, state] = state_read.get();
        CHECK(result == ERROR_SUCCESS);
        CHECK((state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0U);
    }
    if (write_ready) {
        CHECK(output_write.get() == ERROR_SUCCESS);
    }
}

TEST_CASE("runtime.session.blocked_event_history_does_not_hold_ready_service_admission",
    "[integration][runtime][session][progress][concurrency]") {
    session_fixture fixture;
    const auto history = std::make_shared<native_history_script>();
    history->block_next();
    fixture.configure_device(history);
    REQUIRE(fixture.start().has_value());
    fixture.session->activate();
    REQUIRE(fixture.wait_for_presentation());
    REQUIRE(fixture.native->fake->active_registrations(
                fake_gameinput::registration_kind::reading) != 0U);
    REQUIRE(history->wait_until_blocked(wait_limit));

    auto state_read = std::async(std::launch::async, [&fixture] {
        XINPUT_STATE state{};
        return std::pair{fixture.read_state(state), state};
    });
    const history_release_guard release_history{history};

    const auto read_ready = state_read.wait_for(wait_limit) == std::future_status::ready;
    CHECK(read_ready);
    if (read_ready) {
        const auto [result, state] = state_read.get();
        CHECK(result == ERROR_SUCCESS);
        CHECK((state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0U);
    }
}

} // namespace inputnext::runtime_session_test_support
