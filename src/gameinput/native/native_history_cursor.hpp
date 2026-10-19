#pragma once

#include "gameinput/history/clock_mapping.hpp"
#include "native_gameinput.hpp"
#include "native_history.hpp"
#include "native_history_api.hpp"

#include <cstdint>
#include <memory>

namespace inputnext::gameinput_detail {

class native_history_cursor_impl final : public native_history_cursor {
public:
    native_history_cursor_impl(std::unique_ptr<native_history_api> api,
        native_device_handle device) noexcept;
    ~native_history_cursor_impl() override;

    native_history_cursor_impl(const native_history_cursor_impl&) = delete;
    native_history_cursor_impl& operator=(const native_history_cursor_impl&) = delete;
    native_history_cursor_impl(native_history_cursor_impl&&) = delete;
    native_history_cursor_impl& operator=(native_history_cursor_impl&&) = delete;

    [[nodiscard]] native_result<native_history_seed> start() noexcept override;
    [[nodiscard]] native_history_step next() noexcept override;

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

    [[nodiscard]] native_history_step failure_step(native_history_read read) noexcept;

    std::unique_ptr<native_history_api> api_;
    native_device_handle device_{};
    reading_reference reference_;
    history_clock_anchor anchor_{};
    std::uint64_t last_observed_timestamp_us_{};
    std::uint64_t last_reading_timestamp_us_{};
    std::uint64_t timeline_floor_qpc_{};
    std::uint64_t checkpoint_qpc_{};
    bool session_retained_{};
    bool initialized_{};
};

} // namespace inputnext::gameinput_detail
