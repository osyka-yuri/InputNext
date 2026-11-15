#pragma once

#include <Windows.h>

#include <expected>
#include <memory>
#include <mutex>

namespace inputnext::dinput {

class event_registration final {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<event_registration>, HRESULT> duplicate(
        HANDLE event);

    ~event_registration();

    event_registration(const event_registration&) = delete;
    event_registration& operator=(const event_registration&) = delete;
    event_registration(event_registration&&) = delete;
    event_registration& operator=(event_registration&&) = delete;

    void signal() noexcept;
    void disable() noexcept;

private:
    explicit event_registration(HANDLE event) noexcept : event_(event) {}

    HANDLE event_{};
    std::mutex signal_gate_;
    bool disabled_{};
};

} // namespace inputnext::dinput
