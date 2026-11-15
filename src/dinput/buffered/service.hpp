#pragma once

#include "subscription.hpp"

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

namespace detail {
struct buffered_input_test_access;
}

// Owns the bounded history lanes backing DirectInput buffered subscriptions. The referenced
// session services must outlive this object and its drain operation.
class buffered_input final {
public:
    buffered_input(core::service& core, gameinput_runtime& runtime, focus::observer& focus);
    ~buffered_input();

    buffered_input(const buffered_input&) = delete;
    buffered_input& operator=(const buffered_input&) = delete;
    buffered_input(buffered_input&&) = delete;
    buffered_input& operator=(buffered_input&&) = delete;

    [[nodiscard]] std::expected<std::shared_ptr<buffered_subscription>, HRESULT> subscribe(
        buffer_configuration configuration) noexcept;

    void close_admission() noexcept;
    void drain() noexcept;
    void refresh_eligibility() noexcept;

private:
    friend struct detail::buffered_input_test_access;
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::dinput
