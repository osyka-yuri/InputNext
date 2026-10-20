#include <inputnext/focus.hpp>

#include <Windows.h>

#include <cstdint>
#include <memory>
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
    current_process_source() : process_(current_process_identity()) {}

    [[nodiscard]] process_instance process() const noexcept override { return process_; }

    [[nodiscard]] reading observe() noexcept override {
        SetLastError(ERROR_SUCCESS);
        HWND const foreground_window = GetForegroundWindow();
        if (foreground_window == nullptr) {
            return reading{.process = process_, .state = evidence::unavailable, .native_code = 0U};
        }

        DWORD foreground_process_id{};
        SetLastError(ERROR_SUCCESS);
        const auto thread_id = GetWindowThreadProcessId(foreground_window, &foreground_process_id);
        const auto native_code = thread_id == 0U ? static_cast<std::uint32_t>(GetLastError()) : 0U;
        if (thread_id == 0U || foreground_process_id == 0U) {
            return reading{
                .process = process_,
                .state = evidence::unavailable,
                .native_code = native_code,
            };
        }

        return reading{
            .process = process_,
            .state =
                foreground_process_id == process_.pid ? evidence::foreground : evidence::background,
            .native_code = native_code,
        };
    }

private:
    const process_instance process_;
};

} // namespace

std::unique_ptr<source> make_current_process_source() {
    return std::make_unique<current_process_source>();
}

} // namespace inputnext::focus
