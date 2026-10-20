#pragma once

#include "xinput/keystroke_mapping.hpp"

#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace inputnext::xinput::detail {

enum class keystroke_operation_result : std::uint8_t {
    applied,
    capacity_exceeded,
    invalid_time,
};

struct keystroke_sample final {
    key_mask mask{};
    std::uint64_t qpc{};
};

class keystroke_repeat_timing final {
public:
    [[nodiscard]] static std::optional<keystroke_repeat_timing> from_qpc_frequency(
        std::uint64_t frequency) noexcept;

    [[nodiscard]] std::uint64_t initial_repeat_floor_ticks() const noexcept {
        return initial_repeat_floor_ticks_;
    }

    [[nodiscard]] std::uint64_t initial_repeat_ceil_ticks() const noexcept {
        return initial_repeat_ceil_ticks_;
    }

    [[nodiscard]] std::uint64_t repeat_floor_ticks() const noexcept { return repeat_floor_ticks_; }

    [[nodiscard]] std::uint64_t repeat_ceil_ticks() const noexcept { return repeat_ceil_ticks_; }

private:
    struct timing_ticks final {
        std::uint64_t initial_floor{};
        std::uint64_t initial_ceil{};
        std::uint64_t repeat_floor{};
        std::uint64_t repeat_ceil{};
    };

    explicit keystroke_repeat_timing(timing_ticks ticks) noexcept
        : initial_repeat_floor_ticks_(ticks.initial_floor),
          initial_repeat_ceil_ticks_(ticks.initial_ceil),
          repeat_floor_ticks_(ticks.repeat_floor),
          repeat_ceil_ticks_(ticks.repeat_ceil) {}

    const std::uint64_t initial_repeat_floor_ticks_;
    const std::uint64_t initial_repeat_ceil_ticks_;
    const std::uint64_t repeat_floor_ticks_;
    const std::uint64_t repeat_ceil_ticks_;
};

class keystroke_tracker final {
public:
    static constexpr std::size_t queue_capacity = 64U;

    keystroke_tracker() noexcept = default;

    void clear() noexcept;

    [[nodiscard]] keystroke_operation_result silent_seed(keystroke_sample sample) noexcept;

    [[nodiscard]] keystroke_operation_result checked_apply_transitions(
        keystroke_sample sample) noexcept;

    [[nodiscard]] keystroke_operation_result checked_advance_repeats(std::uint64_t qpc,
        bool inclusive,
        const keystroke_repeat_timing& timing,
        key_mask& per_turn_repeat_mask) noexcept;

    [[nodiscard]] bool pop(std::uint32_t user_index, XINPUT_KEYSTROKE& output) noexcept;

private:
    struct key_state final {
        std::uint64_t repeat_origin_qpc{};
        bool initial_repeat_pending{};
    };

    struct queued_event final {
        WORD virtual_key{};
        WORD flags{};
    };

    [[nodiscard]] bool accepts_qpc(std::uint64_t qpc) const noexcept;
    void enqueue(WORD virtual_key, WORD flags) noexcept;

    std::array<key_state, keystroke_key_count> keys_{};
    key_mask held_keys_{};
    std::array<queued_event, queue_capacity> queue_{};
    std::size_t queue_head_{};
    std::size_t queue_size_{};
    std::uint64_t last_qpc_{};
};

} // namespace inputnext::xinput::detail
