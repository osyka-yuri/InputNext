#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/native/native_history.hpp"
#include "history/cursor.hpp"
#include "history_capture.hpp"
#include "history_capture_log.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::gameinput_detail {

class source_history_cursor final : public history_detail::history_cursor {
public:
    using clock_source = history_time (*)() noexcept;

    source_history_cursor(native_gameinput& native,
        native_device_handle device,
        history_capture& capture);
    ~source_history_cursor() override = default;

    source_history_cursor(const source_history_cursor&) = delete;
    source_history_cursor& operator=(const source_history_cursor&) = delete;
    source_history_cursor(source_history_cursor&&) = delete;
    source_history_cursor& operator=(source_history_cursor&&) = delete;

    [[nodiscard]] history_result<history_detail::cursor_seed> start(
        history_time now) noexcept override;
    [[nodiscard]] history_detail::cursor_step next(history_time now) noexcept override;

private:
    friend struct source_history_cursor_test_access;

    struct native_diagnostic final {
        std::uint32_t code{};
    };

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>>
    create_native_cursor() noexcept;
    [[nodiscard]] history_result<history_detail::cursor_seed>
    start_native(const native_history_seed& seed, const capture_snapshot& snapshot) noexcept;
    [[nodiscard]] history_result<history_detail::cursor_seed> start_without_native(
        const capture_snapshot& snapshot,
        native_failure native_failure_value) noexcept;
    [[nodiscard]] history_result<history_detail::cursor_seed>
    start_fallback(const capture_snapshot& snapshot, native_failure native_failure_value) noexcept;
    [[nodiscard]] history_detail::cursor_step next_native() noexcept;
    [[nodiscard]] std::optional<history_detail::cursor_step> read_native_capture() noexcept;
    [[nodiscard]] history_detail::cursor_step merge_native_step() noexcept;
    [[nodiscard]] history_detail::cursor_step next_fallback(history_time now) noexcept;
    [[nodiscard]] history_detail::cursor_step try_native_promotion() noexcept;
    [[nodiscard]] history_detail::cursor_step emit_guide() noexcept;
    [[nodiscard]] history_detail::cursor_step emit_native(const native_history_step& step) noexcept;
    [[nodiscard]] history_detail::cursor_step emit_fallback(const capture_record& record) noexcept;
    [[nodiscard]] static history_detail::cursor_step make_step(
        history_detail::cursor_step_kind kind) noexcept;
    [[nodiscard]] static history_detail::cursor_step
    make_step(history_detail::cursor_step_kind kind, native_diagnostic diagnostic) noexcept;
    [[nodiscard]] static history_detail::cursor_step failure_step(
        const native_failure& failure_value) noexcept;
    [[nodiscard]] static history_detail::cursor_step failure_step(
        const native_history_step& step) noexcept;
    void advance_capture_position() noexcept;
    [[nodiscard]] bool caught_up_is_qualified() const noexcept;
    void reset_progress() noexcept;
    void schedule_native_retry(history_time completion) noexcept;
    [[nodiscard]] history_time current_time() const noexcept;

    native_gameinput& native_;
    native_device_handle device_;
    history_capture& capture_;
    std::unique_ptr<native_history_cursor> native_cursor_;
    std::optional<native_history_step> pending_native_;
    std::optional<capture_record> pending_guide_;
    std::optional<history_time> native_retry_at_;
    std::uint64_t capture_position_{1U};
    std::uint64_t promotion_end_position_{};
    std::uint64_t coverage_generation_{};
    std::uint64_t qpc_frequency_{};
    std::uint64_t timeline_floor_qpc_{};
    std::uint64_t fallback_horizon_qpc_{};
    std::uint64_t source_timestamp_us_{};
    gamepad_state current_pad_{};
    bool fallback_mode_{};
    bool callback_coverage_{};
    bool guide_fresh_{};
    bool guide_known_{};
    bool guide_pressed_{};
    bool initialized_{};
    bool promotion_pending_{};
    bool capture_position_exhausted_{};
    clock_source clock_{&steady_clock_now};

    [[nodiscard]] static history_time steady_clock_now() noexcept;
};

} // namespace inputnext::gameinput_detail
