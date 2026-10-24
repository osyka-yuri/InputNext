#pragma once

#include "manual_sequence.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace inputnext::gameinput_detail {

struct rumble_window_state;

struct rumble_window_options final {
    std::wstring_view title{L"InputNext GameInput Rumble Diagnostic"};
    std::wstring_view instructions{
        L"Click Start, then check each effect before choosing Works or Does not work. "
        L"Keep this window in the foreground. Abort stops the sequence."};
};

class rumble_window final {
public:
    explicit rumble_window(rumble_window_options options = {});
    ~rumble_window() noexcept;

    rumble_window(const rumble_window&) = delete;
    rumble_window& operator=(const rumble_window&) = delete;
    rumble_window(rumble_window&&) = delete;
    rumble_window& operator=(rumble_window&&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool pump_messages() noexcept;
    [[nodiscard]] bool take_start_request() noexcept;
    [[nodiscard]] std::optional<manual_step_confirmation> take_step_result() noexcept;
    [[nodiscard]] bool abort_requested() const noexcept;
    [[nodiscard]] bool is_foreground() const noexcept;
    void set_status(std::string_view status) noexcept;
    void set_running(bool running) noexcept;
    void enable_confirmation(manual_confirmation_id readiness_id) noexcept;
    void disable_confirmation() noexcept;
    void close() noexcept;

private:
    std::unique_ptr<rumble_window_state> state_;
};

} // namespace inputnext::gameinput_detail
