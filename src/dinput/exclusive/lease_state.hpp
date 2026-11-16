#pragma once

#include "lease.hpp"

#include <cstdint>
#include <mutex>

namespace inputnext::dinput {

struct exclusive_lease::state final {
    enum class phase : std::uint8_t { reserved, started, prepared, active, closing, closed };

    ~state();

    state(const state&) = delete;
    state& operator=(const state&) = delete;
    state(state&&) = delete;
    state& operator=(state&&) = delete;

    state() noexcept = default;

    static void close_handle(HANDLE value) noexcept;

    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] HRESULT preparation_result() const noexcept;
    [[nodiscard]] HRESULT close_result() const noexcept;
    [[nodiscard]] bool is_live() const noexcept;
    [[nodiscard]] HRESULT start() noexcept;
    [[nodiscard]] bool activate() noexcept;
    void request_cancel() noexcept;
    void publish_prepared(HRESULT result) noexcept;
    void publish_closed(HRESULT result) noexcept;

    mutable std::mutex gate;
    phase current{phase::reserved};
    bool cancel_requested{};
    bool prepared_ready{};
    HRESULT prepared_status{E_PENDING};
    HRESULT close_status{DI_OK};
    HANDLE start_event{};
    HANDLE prepared_event{};
    HANDLE cancellation_event{};
    HANDLE closed_event{};
};

} // namespace inputnext::dinput
