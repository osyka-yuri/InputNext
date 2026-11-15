#pragma once

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>

namespace inputnext::dinput {

class catalogue_discovery final {
    struct state;

public:
    using clock = std::chrono::steady_clock;
    static constexpr auto default_budget = std::chrono::milliseconds{2000};
    static constexpr std::size_t waiter_capacity = 32U;

    class waiter final {
    public:
        ~waiter();
        waiter(const waiter&) = delete;
        waiter& operator=(const waiter&) = delete;
        waiter(waiter&&) = delete;
        waiter& operator=(waiter&&) = delete;

        [[nodiscard]] bool prepare() noexcept;
        [[nodiscard]] HANDLE event() const noexcept;
        [[nodiscard]] clock::time_point deadline() const noexcept;

    private:
        friend class catalogue_discovery;
        explicit waiter(std::shared_ptr<state> owner, HANDLE event) noexcept;

        std::shared_ptr<state> owner_;
        HANDLE event_{};
        waiter* next_{};
    };

    explicit catalogue_discovery(std::chrono::milliseconds budget = default_budget);
    ~catalogue_discovery();
    catalogue_discovery(const catalogue_discovery&) = delete;
    catalogue_discovery& operator=(const catalogue_discovery&) = delete;
    catalogue_discovery(catalogue_discovery&&) = delete;
    catalogue_discovery& operator=(catalogue_discovery&&) = delete;

    [[nodiscard]] std::unique_ptr<waiter> begin();
    void notify() noexcept;
    void close() noexcept;

private:
    struct state final {
        explicit state(std::chrono::milliseconds requested_budget) noexcept
            : budget(requested_budget) {}

        std::mutex gate;
        std::chrono::milliseconds budget;
        clock::time_point deadline;
        waiter* waiters{};
        std::size_t waiter_count{};
        bool closed{};
    };

    friend struct catalogue_discovery_test_access;
    std::shared_ptr<state> state_;
};

} // namespace inputnext::dinput
