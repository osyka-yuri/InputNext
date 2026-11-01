#pragma once

#include <inputnext/output.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace inputnext::gameinput_detail {

enum class rumble_step_action : std::uint8_t {
    set_state,
    disable,
    set_disabled_cache,
    enable,
};

struct rumble_step final {
    std::string_view name;
    rumble_step_action action{};
    std::uint16_t left_motor{};
    std::uint16_t right_motor{};
    bool expects_native_output{};
    bool logical_channel_present{};
    std::string_view instruction;
};

struct rumble_motor_selection final {
    bool low_motor{};
    bool high_motor{};

    [[nodiscard]] bool any() const noexcept { return low_motor || high_motor; }
};

class rumble_sequence final {
public:
    [[nodiscard]] std::span<const rumble_step> steps() const noexcept {
        return {steps_.data(), size_};
    }

    [[nodiscard]] const rumble_step* begin() const noexcept { return steps_.data(); }

    [[nodiscard]] const rumble_step* end() const noexcept { return steps_.data() + size_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    friend rumble_sequence make_rumble_sequence(rumble_motor_selection selection) noexcept;

    std::array<rumble_step, 7U> steps_{};
    std::size_t size_{};
};

[[nodiscard]] rumble_sequence make_rumble_sequence(rumble_motor_selection selection) noexcept;
[[nodiscard]] output::rumble_value normalized_value(const rumble_step& step) noexcept;

} // namespace inputnext::gameinput_detail
