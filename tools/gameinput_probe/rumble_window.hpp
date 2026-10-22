#pragma once

#include <memory>
#include <string_view>

namespace inputnext::gameinput_detail {

struct rumble_window_state;

class rumble_window final {
public:
    rumble_window();
    ~rumble_window() noexcept;

    rumble_window(const rumble_window&) = delete;
    rumble_window& operator=(const rumble_window&) = delete;
    rumble_window(rumble_window&&) = delete;
    rumble_window& operator=(rumble_window&&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool pump_messages() noexcept;
    [[nodiscard]] bool take_start_request() noexcept;
    [[nodiscard]] bool abort_requested() const noexcept;
    [[nodiscard]] bool is_foreground() const noexcept;
    void set_status(std::string_view status) noexcept;
    void set_running(bool running) noexcept;
    void close() noexcept;

private:
    std::unique_ptr<rumble_window_state> state_;
};

} // namespace inputnext::gameinput_detail
