#pragma once

#include <inputnext/gamepad_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace inputnext::input {

// Touch inputs are synthetic physical sources. They never enter the public button mask.
enum class touch_source : std::uint8_t { none, left, right, fallback };

struct physical_frame final {
    gamepad_state pad{};
    touch_source touch{touch_source::none};
};

enum class source : std::uint8_t {
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    menu,
    select,
    left_stick_click,
    right_stick_click,
    left_shoulder,
    right_shoulder,
    face_bottom,
    face_right,
    face_left,
    face_top,
    left_trigger,
    right_trigger,
    touch_left,
    touch_right,
    touch_fallback,
    count,
};

[[nodiscard]] constexpr bool is_trigger_source(const source input) noexcept {
    return input == source::left_trigger || input == source::right_trigger;
}

enum class target_kind : std::uint8_t { disabled, button, trigger };

struct route final {
    target_kind kind{target_kind::disabled};
    std::optional<gamepad_button> button = std::nullopt;
    bool right_trigger{};
    double threshold{0.5};

    friend constexpr bool operator==(const route&, const route&) noexcept = default;
};

class program_builder;

// A compiled program owns a fixed route table and is safe to share across readers.
class program final {
public:
    program() noexcept;

    [[nodiscard]] static program identity() noexcept;
    [[nodiscard]] static program defaults() noexcept;
    [[nodiscard]] bool has_touch_routes() const noexcept;
    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool is_ordinary_identity() const noexcept;
    [[nodiscard]] const route& mapping(source input) const noexcept;

    friend constexpr bool operator==(const program&, const program&) noexcept = default;

private:
    friend class program_builder;
    std::array<route, static_cast<std::size_t>(source::count)> routes_{};
    bool touch_routes_{};
    bool identity_{true};
    bool ordinary_identity_{true};
};

class program_builder final {
public:
    explicit program_builder(program base = program::identity()) noexcept;

    // Returns false for an invalid source/destination combination or threshold.
    [[nodiscard]] bool set(source input, route output) noexcept;
    [[nodiscard]] std::optional<program> build() const noexcept;

private:
    program value_;
    bool valid_{true};
};

// Projects every source from the same physical frame. Routes never feed other routes.
[[nodiscard]] gamepad_state project(const physical_frame& frame, const program& mappings) noexcept;

} // namespace inputnext::input
