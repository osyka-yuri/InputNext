#pragma once

#include <Windows.h>

#include <chrono>
#include <memory>

namespace inputnext::dinput {

class exclusive_input;

class exclusive_lease final {
public:
    ~exclusive_lease();

    exclusive_lease(const exclusive_lease&) = delete;
    exclusive_lease& operator=(const exclusive_lease&) = delete;
    exclusive_lease(exclusive_lease&&) = delete;
    exclusive_lease& operator=(exclusive_lease&&) = delete;

    [[nodiscard]] HRESULT start() noexcept;
    [[nodiscard]] HRESULT wait_prepared() noexcept;
    [[nodiscard]] bool try_activate() noexcept;
    [[nodiscard]] bool live() const noexcept;
    [[nodiscard]] HRESULT authority_status() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point authority_deadline() const noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] HRESULT wait_closed() noexcept;
    [[nodiscard]] HANDLE cancellation_event() const noexcept;

private:
    friend class exclusive_input;
    struct state;

    explicit exclusive_lease(std::shared_ptr<state> state) noexcept;

    std::shared_ptr<state> state_;
};

} // namespace inputnext::dinput
