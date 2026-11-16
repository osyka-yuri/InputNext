#pragma once

#include "dinput/catalogue/discovery.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace inputnext {
class gameinput_runtime;

namespace core {
class service;
}

namespace focus {
class observer;
}
} // namespace inputnext

namespace inputnext::dinput {

class buffered_input;
class exclusive_input;

// COM objects retain this control block, not the borrowed session dependencies.
// Close admission and drain admitted calls before destroying those dependencies.
class input_access final {
public:
    input_access(core::service& core,
        gameinput_runtime& runtime,
        focus::observer& focus,
        std::chrono::milliseconds discovery_budget = catalogue_discovery::default_budget);
    ~input_access();
    input_access(const input_access&) = delete;
    input_access& operator=(const input_access&) = delete;
    input_access(input_access&&) = delete;
    input_access& operator=(input_access&&) = delete;

    class call final {
    public:
        call() noexcept = default;
        ~call();
        call(call&& other) noexcept;
        call& operator=(call&& other) noexcept;
        call(const call&) = delete;
        call& operator=(const call&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] core::service& core() const noexcept;
        [[nodiscard]] gameinput_runtime& runtime() const noexcept;
        [[nodiscard]] focus::observer& focus() const noexcept;
        [[nodiscard]] buffered_input& buffers() const noexcept;
        [[nodiscard]] exclusive_input& exclusives() const noexcept;

    private:
        friend class input_access;
        explicit call(input_access* owner) noexcept;
        input_access* owner_{};
    };

    [[nodiscard]] call enter() noexcept;
    [[nodiscard]] catalogue_discovery& discovery() noexcept;
    void notify_discovery() noexcept;
    void close_admission() noexcept;
    void drain() noexcept;
    void refresh_eligibility() noexcept;
    [[nodiscard]] bool open() const noexcept;

private:
    void leave() noexcept;
    static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63U;
    std::atomic<std::uint64_t> admission_;
    core::service& core_;
    gameinput_runtime& runtime_;
    focus::observer& focus_;
    catalogue_discovery discovery_;
    std::unique_ptr<buffered_input> buffers_;
    std::unique_ptr<exclusive_input> exclusives_;
};

} // namespace inputnext::dinput
