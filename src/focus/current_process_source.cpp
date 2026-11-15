#include <inputnext/focus.hpp>

#include "window_events.hpp"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace inputnext::focus {
namespace {

[[nodiscard]] process_instance current_process_identity() {
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    HANDLE const process_handle = GetCurrentProcess();
    if (GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time, &user_time) ==
        FALSE) {
        const auto error = GetLastError();
        throw std::system_error{std::error_code{static_cast<int>(error), std::system_category()},
            "GetProcessTimes failed"};
    }

    const auto creation_value = (static_cast<std::uint64_t>(creation_time.dwHighDateTime) << 32U) |
                                static_cast<std::uint64_t>(creation_time.dwLowDateTime);
    const auto pid = GetCurrentProcessId();
    if (pid == 0U || creation_value == 0U) {
        throw std::system_error{std::error_code{ERROR_INVALID_DATA, std::system_category()},
            "current process identity is invalid"};
    }

    return process_instance{
        .pid = static_cast<std::uint32_t>(pid),
        .creation_time_100ns = creation_value,
    };
}

class current_process_source final : public source {
public:
    explicit current_process_source(bool track_windows) : process_(current_process_identity()) {
        if (track_windows) {
            window_events_.emplace();
        }
    }

    [[nodiscard]] process_instance process() const noexcept override { return process_; }

    [[nodiscard]] reading observe() noexcept override {
        const auto before =
            window_events_.has_value() ? window_events_->read() : detail::window_events::sample{};
        SetLastError(ERROR_SUCCESS);
        HWND const foreground_window = GetForegroundWindow();
        if (foreground_window == nullptr) {
            return with_window(
                reading{.process = process_, .state = evidence::unavailable, .native_code = 0U},
                foreground_window,
                0U,
                before);
        }

        DWORD foreground_process_id{};
        SetLastError(ERROR_SUCCESS);
        const auto thread_id = GetWindowThreadProcessId(foreground_window, &foreground_process_id);
        const auto native_code = thread_id == 0U ? static_cast<std::uint32_t>(GetLastError()) : 0U;
        if (thread_id == 0U || foreground_process_id == 0U) {
            return with_window(reading{.process = process_,
                                   .state = evidence::unavailable,
                                   .native_code = native_code},
                foreground_window,
                foreground_process_id,
                before);
        }

        return with_window(reading{.process = process_,
                               .state = foreground_process_id == process_.pid
                                            ? evidence::foreground
                                            : evidence::background,
                               .native_code = native_code},
            foreground_window,
            foreground_process_id,
            before);
    }

private:
    [[nodiscard]] reading with_window(reading result,
        HWND foreground_window,
        DWORD foreground_process_id,
        detail::window_events::sample before) const noexcept {
        if (!window_events_.has_value()) {
            return result;
        }

        const auto after = window_events_->read();
        result.window_transition = after.transition;
        result.window_available =
            before.available && after.available && before.transition == after.transition &&
            foreground_window != nullptr && foreground_process_id == process_.pid;
        if (result.window_available) {
            result.foreground_window = reinterpret_cast<window_id>(foreground_window);
        }
        return result;
    }

    const process_instance process_;
    std::optional<detail::window_events> window_events_;
};

} // namespace

std::unique_ptr<source> make_current_process_source(bool track_windows) {
    return std::make_unique<current_process_source>(track_windows);
}

} // namespace inputnext::focus
