#include "xinput/keystroke_mapping.hpp"
#include "xinput/keystroke_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace inputnext::xinput::detail;

namespace {

[[nodiscard]] constexpr key_mask key_at(std::uint32_t index) noexcept {
    return key_mask{1U} << index;
}

constexpr key_mask all_keys = std::numeric_limits<key_mask>::max();

constexpr std::array<WORD, 32U> expected_virtual_keys{
    0x5810U,
    0x5811U,
    0x5812U,
    0x5813U,
    0x5814U,
    0x5815U,
    0x5816U,
    0x5817U,
    0x5805U,
    0x5804U,
    0x5800U,
    0x5801U,
    0x5802U,
    0x5803U,
    0x5806U,
    0x5807U,
    0x5820U,
    0x5821U,
    0x5822U,
    0x5823U,
    0x5824U,
    0x5825U,
    0x5826U,
    0x5827U,
    0x5830U,
    0x5831U,
    0x5832U,
    0x5833U,
    0x5834U,
    0x5835U,
    0x5836U,
    0x5837U,
};

[[nodiscard]] XINPUT_GAMEPAD left_stick(std::array<SHORT, 2U> axes) noexcept {
    XINPUT_GAMEPAD gamepad{};
    gamepad.sThumbLX = axes[0U];
    gamepad.sThumbLY = axes[1U];
    return gamepad;
}

[[nodiscard]] XINPUT_GAMEPAD right_stick(std::array<SHORT, 2U> axes) noexcept {
    XINPUT_GAMEPAD gamepad{};
    gamepad.sThumbRX = axes[0U];
    gamepad.sThumbRY = axes[1U];
    return gamepad;
}

[[nodiscard]] keystroke_repeat_timing valid_repeat_timing(std::uint64_t frequency) {
    auto timing = keystroke_repeat_timing::from_qpc_frequency(frequency);
    if (!timing.has_value()) {
        throw std::runtime_error("expected a valid QPC frequency");
    }
    return timing.value();
}

} // namespace

TEST_CASE("keystroke mapping preserves supported button order and excludes Guide",
    "[unit][xinput][keystroke]") {
    constexpr std::array<WORD, 14U> button_masks{
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
    };

    for (std::uint32_t index = 0U; index < button_masks.size(); ++index) {
        XINPUT_GAMEPAD gamepad{};
        gamepad.wButtons = button_masks[index];
        CHECK(map_keystroke_keys(gamepad) == key_at(index));
    }

    XINPUT_GAMEPAD gamepad{};
    gamepad.wButtons = 0x0400U; // Guide.
    CHECK(map_keystroke_keys(gamepad) == 0U);

    gamepad.wButtons = 0x0C00U;
    CHECK(map_keystroke_keys(gamepad) == 0U);
}

TEST_CASE("keystroke mapping uses strict trigger and inclusive deadzone boundaries",
    "[unit][xinput][keystroke]") {
    XINPUT_GAMEPAD gamepad{};
    gamepad.bLeftTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    gamepad.bRightTrigger = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    CHECK(map_keystroke_keys(gamepad) == 0U);

    gamepad.bLeftTrigger = static_cast<BYTE>(XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1U);
    gamepad.bRightTrigger = static_cast<BYTE>(XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1U);
    CHECK(map_keystroke_keys(gamepad) == (key_at(14U) | key_at(15U)));

    CHECK(map_keystroke_keys(left_stick({XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, 0})) == 0U);
    CHECK(map_keystroke_keys(left_stick(
              {static_cast<SHORT>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 1), 0})) == key_at(18U));
    CHECK(map_keystroke_keys(right_stick({XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, 0})) == 0U);
    CHECK(map_keystroke_keys(right_stick(
              {static_cast<SHORT>(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE + 1), 0})) == key_at(26U));
}

TEST_CASE("keystroke mapping keeps cardinals, angular cutoffs, diagonals, and SHORT extremes",
    "[unit][xinput][keystroke]") {
    struct direction_case final {
        SHORT x;
        SHORT y;
        std::uint32_t left_key;
    };

    constexpr std::array<direction_case, 8U> directions{
        direction_case{.x = 0, .y = 10000, .left_key = 16U},
        direction_case{.x = 0, .y = -10000, .left_key = 17U},
        direction_case{.x = 10000, .y = 0, .left_key = 18U},
        direction_case{.x = -10000, .y = 0, .left_key = 19U},
        direction_case{.x = -10000, .y = 10000, .left_key = 20U},
        direction_case{.x = 10000, .y = 10000, .left_key = 21U},
        direction_case{.x = 10000, .y = -10000, .left_key = 22U},
        direction_case{.x = -10000, .y = -10000, .left_key = 23U},
    };

    for (const auto& direction : directions) {
        CHECK(map_keystroke_keys(left_stick({direction.x, direction.y})) ==
              key_at(direction.left_key));
        CHECK(map_keystroke_keys(right_stick({direction.x, direction.y})) ==
              key_at(direction.left_key + 8U));
    }

    CHECK(map_keystroke_keys(left_stick({10000, 4142})) == key_at(18U));
    CHECK(map_keystroke_keys(left_stick({10000, 4143})) == key_at(21U));
    CHECK(map_keystroke_keys(left_stick({10000, 24142})) == key_at(21U));
    CHECK(map_keystroke_keys(left_stick({10000, 24143})) == key_at(16U));
    CHECK(map_keystroke_keys(left_stick({-10000, 4143})) == key_at(20U));
    CHECK(map_keystroke_keys(left_stick({10000, -4143})) == key_at(22U));
    CHECK(map_keystroke_keys(left_stick({-10000, -4143})) == key_at(23U));

    CHECK(map_keystroke_keys(left_stick({std::numeric_limits<SHORT>::min(),
              std::numeric_limits<SHORT>::max()})) == key_at(20U));
    CHECK(map_keystroke_keys(right_stick({std::numeric_limits<SHORT>::max(),
              std::numeric_limits<SHORT>::min()})) == key_at(30U));
}

TEST_CASE("keystroke tracker emits all 32 virtual keys in frozen order with clean output fields",
    "[unit][xinput][keystroke]") {
    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = 0U, .qpc = 1U}) == keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = all_keys, .qpc = 2U}) ==
            keystroke_operation_result::applied);

    for (const auto expected_key : expected_virtual_keys) {
        XINPUT_KEYSTROKE output{};
        REQUIRE(tracker.pop(3U, output));
        CHECK(output.VirtualKey == expected_key);
        CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
        CHECK(output.UserIndex == 3U);
        CHECK(output.Unicode == 0U);
        CHECK(output.HidCode == 0U);
    }

    XINPUT_KEYSTROKE empty_output{};
    CHECK(!tracker.pop(3U, empty_output));
}

TEST_CASE("keystroke tracker seeds held keys silently and leaves failed pop output untouched",
    "[unit][xinput][keystroke]") {
    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = key_at(10U), .qpc = 500U}) ==
            keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    output.VirtualKey = 0xAAAAU;
    output.Unicode = static_cast<WCHAR>(0xBBBBU);
    output.Flags = 0xCCCCU;
    output.UserIndex = 0xDDU;
    output.HidCode = 0xEEU;
    CHECK(!tracker.pop(0U, output));
    CHECK(output.VirtualKey == 0xAAAAU);
    CHECK(output.Unicode == static_cast<WCHAR>(0xBBBBU));
    CHECK(output.Flags == 0xCCCCU);
    CHECK(output.UserIndex == 0xDDU);
    CHECK(output.HidCode == 0xEEU);

    const auto timing = valid_repeat_timing(1000U);
    key_mask per_turn_repeat_mask{};
    CHECK(tracker.checked_advance_repeats(999U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    XINPUT_KEYSTROKE repeat_output{};
    CHECK(!tracker.pop(0U, repeat_output));
}

TEST_CASE("keystroke repeat timing uses inclusive checkpoints and exclusive event horizons",
    "[unit][xinput][keystroke]") {
    const auto timing = valid_repeat_timing(1000U);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 100U}) ==
            keystroke_operation_result::applied);
    key_mask per_turn_repeat_mask{};

    CHECK(tracker.checked_advance_repeats(599U, false, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(tracker.checked_advance_repeats(600U, false, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    CHECK(!tracker.pop(0U, output));

    CHECK(tracker.checked_advance_repeats(600U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));
    CHECK(output.VirtualKey == expected_virtual_keys[0U]);
    CHECK(output.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));

    per_turn_repeat_mask = 0U;
    CHECK(tracker.checked_advance_repeats(699U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(!tracker.pop(0U, output));
    CHECK(tracker.checked_advance_repeats(700U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));
    CHECK(output.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));
}

TEST_CASE("an event at the repeat deadline precedes an inclusive checkpoint repeat",
    "[unit][xinput][keystroke]") {
    const auto timing = valid_repeat_timing(1000U);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 100U}) ==
            keystroke_operation_result::applied);
    key_mask per_turn_repeat_mask{};
    CHECK(tracker.checked_advance_repeats(600U, false, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(tracker.checked_apply_transitions({.mask = 0U, .qpc = 600U}) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    REQUIRE(tracker.pop(0U, output));
    CHECK(output.VirtualKey == expected_virtual_keys[0U]);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYUP);
    CHECK(!tracker.pop(0U, output));

    CHECK(tracker.checked_advance_repeats(600U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(!tracker.pop(0U, output));
}

TEST_CASE("keystroke repeat timing rounds nondivisible and odd low QPC frequencies safely",
    "[unit][xinput][keystroke]") {
    const auto nondivisible_timing = valid_repeat_timing(1001U);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 10U}) ==
            keystroke_operation_result::applied);
    key_mask per_turn_repeat_mask{};
    CHECK(tracker.checked_advance_repeats(510U, true, nondivisible_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    XINPUT_KEYSTROKE output{};
    CHECK(!tracker.pop(0U, output));
    CHECK(tracker.checked_advance_repeats(511U, true, nondivisible_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));

    per_turn_repeat_mask = 0U;
    CHECK(tracker.checked_advance_repeats(611U, true, nondivisible_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(!tracker.pop(0U, output));
    CHECK(tracker.checked_advance_repeats(612U, true, nondivisible_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));

    const auto low_timing = valid_repeat_timing(3U);
    tracker.clear();
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 1U}) ==
            keystroke_operation_result::applied);
    per_turn_repeat_mask = 0U;
    CHECK(tracker.checked_advance_repeats(2U, true, low_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(!tracker.pop(0U, output));
    CHECK(tracker.checked_advance_repeats(3U, true, low_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));

    per_turn_repeat_mask = 0U;
    CHECK(tracker.checked_advance_repeats(4U, true, low_timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));
}

TEST_CASE("keystroke repeat timing avoids absolute deadline overflow near the QPC maximum",
    "[unit][xinput][keystroke]") {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto timing = valid_repeat_timing(maximum);

    keystroke_tracker tracker;
    constexpr auto origin = (std::uint64_t{1} << 63U) - 1U;
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = origin}) ==
            keystroke_operation_result::applied);
    key_mask per_turn_repeat_mask{};
    CHECK(tracker.checked_advance_repeats(maximum, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    REQUIRE(tracker.pop(0U, output));
    CHECK(output.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));
}

TEST_CASE("keystroke repeat advancement coalesces once per key per turn across horizons",
    "[unit][xinput][keystroke]") {
    const auto timing = valid_repeat_timing(1000U);

    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 100U}) ==
            keystroke_operation_result::applied);
    key_mask per_turn_repeat_mask{};

    CHECK(tracker.checked_advance_repeats(600U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(per_turn_repeat_mask == key_at(0U));
    CHECK(tracker.checked_advance_repeats(700U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    CHECK(tracker.checked_advance_repeats(700U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    REQUIRE(tracker.pop(0U, output));
    CHECK(!tracker.pop(0U, output));

    per_turn_repeat_mask = 0U;
    CHECK(tracker.checked_advance_repeats(800U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);
    REQUIRE(tracker.pop(0U, output));
    CHECK(output.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));
}

TEST_CASE("keystroke transition groups fail atomically when the queue is full",
    "[unit][xinput][keystroke]") {
    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = 0U, .qpc = 10U}) == keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = all_keys, .qpc = 11U}) ==
            keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = 0U, .qpc = 12U}) ==
            keystroke_operation_result::applied);
    CHECK(tracker.checked_apply_transitions({.mask = all_keys, .qpc = 30U}) ==
          keystroke_operation_result::capacity_exceeded);

    for (const auto expected_key : expected_virtual_keys) {
        XINPUT_KEYSTROKE output{};
        REQUIRE(tracker.pop(0U, output));
        CHECK(output.VirtualKey == expected_key);
        CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
    }
    for (const auto virtual_key : expected_virtual_keys) {
        XINPUT_KEYSTROKE output{};
        REQUIRE(tracker.pop(0U, output));
        CHECK(output.VirtualKey == virtual_key);
        CHECK(output.Flags == XINPUT_KEYSTROKE_KEYUP);
    }

    CHECK(tracker.checked_apply_transitions({.mask = all_keys, .qpc = 29U}) ==
          keystroke_operation_result::applied);
    for (const auto expected_key : expected_virtual_keys) {
        XINPUT_KEYSTROKE output{};
        REQUIRE(tracker.pop(0U, output));
        CHECK(output.VirtualKey == expected_key);
        CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
    }
}

TEST_CASE("keystroke repeat groups fail atomically when the queue is full",
    "[unit][xinput][keystroke]") {
    const auto timing = valid_repeat_timing(1000U);

    constexpr key_mask first_half = 0x0000FFFFU;
    keystroke_tracker tracker;
    REQUIRE(tracker.silent_seed({.mask = first_half, .qpc = 100U}) ==
            keystroke_operation_result::applied);
    key_mask first_turn_mask{};
    REQUIRE(tracker.checked_advance_repeats(600U, true, timing, first_turn_mask) ==
            keystroke_operation_result::applied);

    REQUIRE(tracker.checked_apply_transitions({.mask = 0U, .qpc = 601U}) ==
            keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = first_half, .qpc = 602U}) ==
            keystroke_operation_result::applied);
    REQUIRE(tracker.checked_apply_transitions({.mask = all_keys, .qpc = 603U}) ==
            keystroke_operation_result::applied);

    key_mask per_turn_repeat_mask{};
    REQUIRE(tracker.checked_advance_repeats(1102U, true, timing, per_turn_repeat_mask) ==
            keystroke_operation_result::capacity_exceeded);
    CHECK(per_turn_repeat_mask == 0U);
    CHECK(tracker.checked_advance_repeats(1101U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE output{};
    for (std::size_t index = 0U; index < keystroke_tracker::queue_capacity / 4U; ++index) {
        REQUIRE(tracker.pop(0U, output));
    }
    CHECK(tracker.checked_advance_repeats(1102U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::applied);

    XINPUT_KEYSTROKE last{};
    for (std::size_t index = 0U; index < keystroke_tracker::queue_capacity - 16U; ++index) {
        REQUIRE(tracker.pop(0U, output));
    }
    for (std::size_t index = 0U; index < 16U; ++index) {
        REQUIRE(tracker.pop(0U, last));
        CHECK(last.VirtualKey == expected_virtual_keys[index]);
        CHECK(last.Flags == (XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));
    }
    CHECK(!tracker.pop(0U, output));
}

TEST_CASE("keystroke tracker rejects zero and regressing QPC without changing queued state",
    "[unit][xinput][keystroke]") {
    keystroke_tracker tracker;
    CHECK(tracker.checked_apply_transitions({.mask = key_at(0U), .qpc = 10U}) ==
          keystroke_operation_result::invalid_time);
    CHECK(tracker.silent_seed({.mask = key_at(0U), .qpc = 0U}) ==
          keystroke_operation_result::invalid_time);

    REQUIRE(tracker.silent_seed({.mask = key_at(0U), .qpc = 10U}) ==
            keystroke_operation_result::applied);
    CHECK(tracker.checked_apply_transitions({.mask = key_at(1U), .qpc = 0U}) ==
          keystroke_operation_result::invalid_time);
    key_mask per_turn_repeat_mask{};
    const auto timing = valid_repeat_timing(1000U);
    CHECK(tracker.checked_advance_repeats(0U, true, timing, per_turn_repeat_mask) ==
          keystroke_operation_result::invalid_time);
    CHECK(tracker.checked_apply_transitions({.mask = key_at(1U), .qpc = 9U}) ==
          keystroke_operation_result::invalid_time);
    CHECK(tracker.silent_seed({.mask = key_at(2U), .qpc = 0U}) ==
          keystroke_operation_result::invalid_time);
    CHECK(tracker.silent_seed({.mask = key_at(2U), .qpc = 9U}) ==
          keystroke_operation_result::invalid_time);
    XINPUT_KEYSTROKE empty_output{};
    CHECK(!tracker.pop(0U, empty_output));

    CHECK(tracker.checked_apply_transitions({.mask = key_at(1U), .qpc = 10U}) ==
          keystroke_operation_result::applied);
    XINPUT_KEYSTROKE output{};
    REQUIRE(tracker.pop(1U, output));
    CHECK(output.VirtualKey == expected_virtual_keys[0U]);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYUP);
    REQUIRE(tracker.pop(1U, output));
    CHECK(output.VirtualKey == expected_virtual_keys[1U]);
    CHECK(output.Flags == XINPUT_KEYSTROKE_KEYDOWN);
}
