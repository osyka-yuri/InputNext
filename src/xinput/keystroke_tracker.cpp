#include "xinput/keystroke_tracker.hpp"

#include "keystroke_mapping.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace inputnext::xinput::detail {
namespace {

struct tick_interval final {
    std::uint64_t floor{};
    std::uint64_t ceil{};
};

[[nodiscard]] bool milliseconds_to_ticks(std::uint64_t frequency,
    std::chrono::milliseconds duration,
    tick_interval& output) noexcept {
    constexpr std::uint64_t ticks_per_second = 1000U;
    if (duration.count() <= 0) {
        return false;
    }

    const auto milliseconds = static_cast<std::uint64_t>(duration.count());
    const auto quotient = frequency / ticks_per_second;
    const auto remainder = frequency % ticks_per_second;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();

    if (quotient > maximum / milliseconds) {
        return false;
    }

    const auto whole = quotient * milliseconds;
    const auto fraction = remainder * milliseconds;
    const auto fractional_ticks = fraction / ticks_per_second;
    if (whole > maximum - fractional_ticks) {
        return false;
    }

    const auto floor = whole + fractional_ticks;
    const bool has_fraction = (fraction % ticks_per_second) != 0U;
    if (has_fraction && floor == maximum) {
        return false;
    }

    output = {.floor = floor, .ceil = floor + static_cast<std::uint64_t>(has_fraction)};
    return true;
}

// Preserve the XInput virtual-key values without requiring Windows 8-gated VK_PAD_* macros.
constexpr std::array<WORD, keystroke_key_count> virtual_keys{
    0x5810U, // D-pad up
    0x5811U, // D-pad down
    0x5812U, // D-pad left
    0x5813U, // D-pad right
    0x5814U, // Start
    0x5815U, // Back
    0x5816U, // Left thumb press
    0x5817U, // Right thumb press
    0x5805U, // Left shoulder
    0x5804U, // Right shoulder
    0x5800U, // A
    0x5801U, // B
    0x5802U, // X
    0x5803U, // Y
    0x5806U, // Left trigger
    0x5807U, // Right trigger
    0x5820U, // Left thumb up
    0x5821U, // Left thumb down
    0x5822U, // Left thumb right
    0x5823U, // Left thumb left
    0x5824U, // Left thumb up-left
    0x5825U, // Left thumb up-right
    0x5826U, // Left thumb down-right
    0x5827U, // Left thumb down-left
    0x5830U, // Right thumb up
    0x5831U, // Right thumb down
    0x5832U, // Right thumb right
    0x5833U, // Right thumb left
    0x5834U, // Right thumb up-left
    0x5835U, // Right thumb up-right
    0x5836U, // Right thumb down-right
    0x5837U, // Right thumb down-left
};

[[nodiscard]] constexpr key_mask key_bit(std::size_t index) noexcept {
    return key_mask{1U} << index;
}

[[nodiscard]] bool interval_elapsed(std::uint64_t elapsed,
    std::uint64_t floor,
    std::uint64_t ceil,
    bool inclusive) noexcept {
    return inclusive ? elapsed >= ceil : elapsed > floor;
}

} // namespace

std::optional<keystroke_repeat_timing> keystroke_repeat_timing::from_qpc_frequency(
    std::uint64_t frequency) noexcept {
    if (frequency == 0U) {
        return std::nullopt;
    }

    tick_interval initial{};
    tick_interval repeat{};
    if (!milliseconds_to_ticks(frequency, std::chrono::milliseconds{500}, initial) ||
        !milliseconds_to_ticks(frequency, std::chrono::milliseconds{100}, repeat)) {
        return std::nullopt;
    }

    return std::optional<keystroke_repeat_timing>{keystroke_repeat_timing{timing_ticks{
        .initial_floor = initial.floor,
        .initial_ceil = initial.ceil,
        .repeat_floor = repeat.floor,
        .repeat_ceil = repeat.ceil,
    }}};
}

void keystroke_tracker::clear() noexcept {
    keys_ = {};
    held_keys_ = 0U;
    queue_ = {};
    queue_head_ = 0U;
    queue_size_ = 0U;
    last_qpc_ = 0U;
}

keystroke_operation_result keystroke_tracker::silent_seed(keystroke_sample sample) noexcept {
    const auto mask = sample.mask;
    const auto qpc = sample.qpc;
    if (qpc == 0U || (last_qpc_ != 0U && qpc < last_qpc_)) {
        return keystroke_operation_result::invalid_time;
    }

    clear();
    held_keys_ = mask;
    for (std::size_t index = 0U; index < keys_.size(); ++index) {
        if ((mask & key_bit(index)) != 0U) {
            keys_[index] = {.repeat_origin_qpc = qpc, .initial_repeat_pending = true};
        }
    }
    last_qpc_ = qpc;
    return keystroke_operation_result::applied;
}

keystroke_operation_result keystroke_tracker::checked_apply_transitions(
    keystroke_sample sample) noexcept {
    const auto mask = sample.mask;
    const auto qpc = sample.qpc;
    if (!accepts_qpc(qpc)) {
        return keystroke_operation_result::invalid_time;
    }

    const auto changed = held_keys_ ^ mask;
    const auto transition_count = static_cast<std::size_t>(std::popcount(changed));
    if (transition_count > queue_capacity - queue_size_) {
        return keystroke_operation_result::capacity_exceeded;
    }

    for (std::size_t index = 0U; index < keys_.size(); ++index) {
        const auto bit = key_bit(index);
        if ((changed & bit) == 0U) {
            continue;
        }

        if ((mask & bit) != 0U) {
            keys_[index] = {.repeat_origin_qpc = qpc, .initial_repeat_pending = true};
            enqueue(virtual_keys[index], XINPUT_KEYSTROKE_KEYDOWN);
        } else {
            keys_[index] = {};
            enqueue(virtual_keys[index], XINPUT_KEYSTROKE_KEYUP);
        }
    }

    held_keys_ = mask;
    last_qpc_ = qpc;
    return keystroke_operation_result::applied;
}

keystroke_operation_result keystroke_tracker::checked_advance_repeats(std::uint64_t qpc,
    bool inclusive,
    const keystroke_repeat_timing& timing,
    key_mask& per_turn_repeat_mask) noexcept {
    if (!accepts_qpc(qpc)) {
        return keystroke_operation_result::invalid_time;
    }

    key_mask due_mask{};
    for (std::size_t index = 0U; index < keys_.size(); ++index) {
        const auto bit = key_bit(index);
        if ((held_keys_ & bit) == 0U) {
            continue;
        }

        const auto& state = keys_[index];
        const auto floor = state.initial_repeat_pending ? timing.initial_repeat_floor_ticks()
                                                        : timing.repeat_floor_ticks();
        const auto ceil = state.initial_repeat_pending ? timing.initial_repeat_ceil_ticks()
                                                       : timing.repeat_ceil_ticks();
        const auto elapsed = qpc - state.repeat_origin_qpc;
        if (interval_elapsed(elapsed, floor, ceil, inclusive)) {
            due_mask |= bit;
        }
    }

    const auto enqueue_mask = due_mask & ~per_turn_repeat_mask;
    const auto repeat_count = static_cast<std::size_t>(std::popcount(enqueue_mask));
    if (repeat_count > queue_capacity - queue_size_) {
        return keystroke_operation_result::capacity_exceeded;
    }

    for (std::size_t index = 0U; index < keys_.size(); ++index) {
        const auto bit = key_bit(index);
        if ((due_mask & bit) == 0U) {
            continue;
        }

        auto& state = keys_[index];
        state.repeat_origin_qpc = qpc;
        state.initial_repeat_pending = false;
        if ((enqueue_mask & bit) != 0U) {
            enqueue(virtual_keys[index],
                static_cast<WORD>(XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT));
        }
    }

    per_turn_repeat_mask |= enqueue_mask;
    last_qpc_ = qpc;
    return keystroke_operation_result::applied;
}

bool keystroke_tracker::pop(std::uint32_t user_index, XINPUT_KEYSTROKE& output) noexcept {
    constexpr std::uint32_t user_count = 4U;
    if (user_index >= user_count || queue_size_ == 0U) {
        return false;
    }

    const auto& event = queue_[queue_head_];
    XINPUT_KEYSTROKE result{};
    result.VirtualKey = event.virtual_key;
    result.Flags = event.flags;
    result.UserIndex = static_cast<BYTE>(user_index);

    output = result;
    queue_head_ = (queue_head_ + 1U) % queue_capacity;
    --queue_size_;
    return true;
}

bool keystroke_tracker::accepts_qpc(std::uint64_t qpc) const noexcept {
    return qpc != 0U && last_qpc_ != 0U && qpc >= last_qpc_;
}

void keystroke_tracker::enqueue(WORD virtual_key, WORD flags) noexcept {
    const auto tail = (queue_head_ + queue_size_) % queue_capacity;
    queue_[tail] = {.virtual_key = virtual_key, .flags = flags};
    ++queue_size_;
}

} // namespace inputnext::xinput::detail
