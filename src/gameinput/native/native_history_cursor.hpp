#pragma once

#include "gameinput/history/clock_mapping.hpp"
#include "gameinput/input_profile.hpp"
#include "input/program.hpp"
#include "input/touch_resolver.hpp"
#include "native_gameinput.hpp"
#include "native_history.hpp"
#include "native_history_api.hpp"

#include <cstdint>
#include <memory>

namespace inputnext::gameinput_detail {

class native_history_cursor_impl final : public native_history_cursor {
public:
    native_history_cursor_impl(std::unique_ptr<native_history_api> api,
        native_device_handle device,
        native_input_profile profile = {},
        input::touch_snapshot touch_context = {}) noexcept;
    ~native_history_cursor_impl() override;

    native_history_cursor_impl(const native_history_cursor_impl&) = delete;
    native_history_cursor_impl& operator=(const native_history_cursor_impl&) = delete;
    native_history_cursor_impl(native_history_cursor_impl&&) = delete;
    native_history_cursor_impl& operator=(native_history_cursor_impl&&) = delete;

    [[nodiscard]] native_result<native_history_seed> start() noexcept override;
    [[nodiscard]] native_history_step next() noexcept override;
    [[nodiscard]] std::uint32_t bootstrap_transitions() const noexcept override;

private:
    class reading_reference final {
    public:
        reading_reference() noexcept = default;
        reading_reference(native_history_api& api, void* reading) noexcept;
        ~reading_reference();

        reading_reference(const reading_reference&) = delete;
        reading_reference& operator=(const reading_reference&) = delete;
        reading_reference(reading_reference&& other) noexcept;
        reading_reference& operator=(reading_reference&& other) noexcept;

        [[nodiscard]] void* get() const noexcept;
        void reset() noexcept;
        void reset(native_history_api& api, void* reading) noexcept;

    private:
        native_history_api* api_{};
        void* reading_{};
    };

    struct bootstrap_reset_point final {
        reading_reference reading;
        native_history_frame frame{};
    };

    struct bootstrap_replay_result final {
        input::touch_snapshot resolved{};
        std::uint32_t transitions{};
    };

    [[nodiscard]] native_history_step failure_step(native_history_read read) noexcept;
    [[nodiscard]] native_result<native_history_seed> start_composite(std::uint64_t qpc_frequency,
        void* current_reading,
        std::uint64_t current_timestamp,
        std::uint64_t baseline_qpc,
        const native_history_frame& current_frame) noexcept;
    [[nodiscard]] native_result<bootstrap_reset_point> find_released_reset(void* current_reading,
        std::uint64_t first_accepted_timestamp) noexcept;
    [[nodiscard]] native_result<bootstrap_replay_result> replay_from_reset(
        bootstrap_reset_point reset_point,
        void* current_reading,
        const native_history_frame& current_frame) noexcept;
    [[nodiscard]] bool read_composite_frame(void* reading, native_history_frame& frame) noexcept;
    [[nodiscard]] native_history_step caught_up(native_history_read read,
        std::uint64_t observed_timestamp) noexcept;
    [[nodiscard]] native_history_step process_next_reading(native_history_read read) noexcept;

    std::unique_ptr<native_history_api> api_;
    native_device_handle device_{};
    native_input_profile profile_{};
    input::touch_snapshot touch_context_{};
    input::touch_resolver touch_resolver_{};
    reading_reference reference_;
    history_clock_anchor anchor_{};
    std::uint64_t last_observed_timestamp_us_{};
    std::uint64_t last_reading_timestamp_us_{};
    std::uint64_t timeline_floor_qpc_{};
    std::uint64_t checkpoint_qpc_{};
    std::uint32_t bootstrap_transitions_{};
    bool session_retained_{};
    bool initialized_{};
    input_fallback_reason last_composite_failure_reason_{input_fallback_reason::none};
};

} // namespace inputnext::gameinput_detail
