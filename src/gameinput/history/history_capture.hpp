#pragma once

#include "gameinput/input_profile.hpp"
#include "gameinput/reading_parts.hpp"
#include "history_capture_log.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

namespace inputnext::gameinput_detail {

struct capture_snapshot final {
    std::optional<history_sample> fallback_baseline;
    std::optional<history_sample> guide_fact;
    std::uint64_t next_position{1U};
    std::uint64_t oldest_position{1U};
    std::uint64_t tail_position{};
    std::uint64_t overwritten_control_position{};
    std::uint64_t coverage_generation{};
    bool callback_coverage{};
    bool guide_fresh{};
    bool closed{};
    bool exhausted{};
    input::touch_snapshot touch{};
    native_input_profile input_profile{};
};

struct input_diagnostics final {
    // This remains the connection's qualified/configured path when one cursor falls back.
    input_history_path configured_path{input_history_path::gamepad};
    // Cursors are independent, so this is the most recently selected path, not a global mode.
    std::optional<input_history_path> last_selected_history;
    input_fallback_reason last_reason{input_fallback_reason::none};
    std::array<std::uint64_t, 9U> counts{};
    native_input_profile profile{};

    friend constexpr bool operator==(const input_diagnostics&,
        const input_diagnostics&) noexcept = default;
};

struct history_capture_test_access;

class history_capture final {
public:
    explicit history_capture(input::program mappings = input::program::identity(),
        native_input_profile profile = {}) noexcept;
    ~history_capture() = default;

    history_capture(const history_capture&) = delete;
    history_capture& operator=(const history_capture&) = delete;
    history_capture(history_capture&&) = delete;
    history_capture& operator=(history_capture&&) = delete;

    void on_callback(const reading_parts& accepted) noexcept;
    void on_seed(const reading_parts& accepted) noexcept;
    void set_callback_coverage(bool active) noexcept;
    void invalidate_touch(const input::touch_snapshot& touch,
        input_fallback_reason reason,
        bool revoke_profile = false) noexcept;
    void record_fallback(input_fallback_reason reason) noexcept;
    void record_selected_history(input_history_path path) noexcept;
    [[nodiscard]] const input::program& projection() const noexcept;
    [[nodiscard]] input_diagnostics diagnostics() const noexcept;
    void close() noexcept;

    [[nodiscard]] capture_snapshot snapshot() const noexcept;
    [[nodiscard]] capture_read read(std::uint64_t position) const noexcept;

private:
    friend struct history_capture_test_access;

    void update_pad(const reading_parts& accepted, bool fallback_cut) noexcept;
    void update_guide_callback(const reading_parts& accepted) noexcept;
    void update_guide_seed(const reading_parts& accepted) noexcept;
    void append(capture_record_kind kind,
        const gamepad_state& pad,
        bool guide_known,
        bool guide_pressed,
        std::uint64_t source_timestamp_us,
        std::uint64_t qpc) noexcept;
    void refresh_fallback_baseline(std::uint64_t source_timestamp_us, std::uint64_t qpc) noexcept;
    void append_cut(capture_record_kind kind,
        std::uint64_t source_timestamp_us,
        std::uint64_t qpc) noexcept;
    void update_touch(const input::touch_snapshot& touch) noexcept;
    void record_reason(input_fallback_reason reason) noexcept;

    const input::program mappings_;
    mutable std::mutex mutex_;
    history_capture_log log_;
    gamepad_state pad_{};
    std::optional<history_sample> fallback_baseline_;
    std::optional<history_sample> guide_fact_;
    std::uint64_t pad_timestamp_us_{};
    std::uint64_t pad_qpc_{};
    std::uint64_t coverage_generation_{};
    bool pad_known_{};
    bool guide_pressed_{};
    bool callback_coverage_{};
    bool guide_fresh_{};
    bool closed_{};
    input::touch_snapshot touch_{};
    native_input_profile input_profile_{};
    input_diagnostics diagnostics_{};
};

} // namespace inputnext::gameinput_detail
