#pragma once

#include "dinput/input_access.hpp"
#include "dinput/native_types.hpp"
#include "lease.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::dinput {

struct exclusive_lease::state final {
    enum class phase : std::uint8_t { reserved, started, prepared, active, closing, closed };

    ~state();

    state(const state&) = delete;
    state& operator=(const state&) = delete;
    state(state&&) = delete;
    state& operator=(state&&) = delete;

    using clock_callback = std::chrono::steady_clock::time_point (*)(const void*) noexcept;

    struct preparation_wait final {
        std::optional<HRESULT> result;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    state(clock_callback now,
        std::shared_ptr<const void> context,
        connection_target target,
        std::optional<focus::window_epoch> window) noexcept;

    static void close_handle(HANDLE value) noexcept;

    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] preparation_wait prepare_wait() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] HRESULT close_result() const noexcept;
    [[nodiscard]] bool is_live() const noexcept;
    [[nodiscard]] HRESULT authority() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point authority_deadline() const noexcept;
    [[nodiscard]] HRESULT record_probe(HRESULT result,
        std::chrono::steady_clock::time_point started,
        bool initial,
        const std::atomic<bool>& admission_closed,
        const input_access& access) noexcept;
    [[nodiscard]] HRESULT start() noexcept;
    [[nodiscard]] bool activate() noexcept;
    void request_cancel() noexcept;
    void publish_prepared(HRESULT result) noexcept;
    void publish_closed(HRESULT result) noexcept;
    [[nodiscard]] std::expected<std::shared_ptr<const output::detail::window_acquisition>, HRESULT>
    rumble_window(connection_target requested, const focus::window_epoch& epoch) noexcept;

    HANDLE start_event{};
    HANDLE prepared_event{};
    HANDLE cancellation_event{};
    HANDLE closed_event{};

private:
    [[nodiscard]] HRESULT record_probe_locked(HRESULT result,
        std::chrono::steady_clock::time_point started,
        bool initial,
        const std::atomic<bool>& admission_closed,
        const input_access& access) noexcept;
    void publish_prepared_locked(HRESULT result) noexcept;
    [[nodiscard]] HRESULT authority_locked() const noexcept;
    void expire_locked() const noexcept;
    void cancel_locked(HRESULT reason) const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point real_deadline(
        std::chrono::steady_clock::time_point deadline) const noexcept;

    const clock_callback clock;
    const std::shared_ptr<const void> clock_context;
    const connection_target target;
    const std::optional<focus::window_epoch> window;
    std::shared_ptr<output::detail::window_acquisition> rumble;
    mutable std::mutex gate;
    mutable phase current{phase::reserved};
    mutable std::optional<HRESULT> terminal;
    std::optional<HRESULT> preparation;
    std::optional<std::chrono::steady_clock::time_point> proof_deadline;
    std::optional<std::chrono::steady_clock::time_point> preparation_deadline;
    HRESULT close_status{DI_OK};
};

} // namespace inputnext::dinput
