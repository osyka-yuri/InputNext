#pragma once

#include <inputnext/output.hpp>

#include <chrono>

namespace inputnext::output {

class finite_submission_window final {
public:
    [[nodiscard]] constexpr std::chrono::steady_clock::time_point entered_at() const noexcept {
        return entered_at_;
    }

    [[nodiscard]] constexpr std::chrono::steady_clock::time_point expires_at() const noexcept {
        return expires_at_;
    }

    [[nodiscard]] constexpr std::chrono::milliseconds ttl() const noexcept { return ttl_; }

private:
    constexpr finite_submission_window(std::chrono::steady_clock::time_point entered_at,
        std::chrono::milliseconds ttl,
        std::chrono::steady_clock::time_point expires_at) noexcept
        : entered_at_(entered_at),
          expires_at_(expires_at),
          ttl_(ttl) {}

    friend result<finite_submission_window> make_finite_submission_window(
        std::chrono::steady_clock::time_point entered_at,
        std::chrono::milliseconds ttl) noexcept;

    std::chrono::steady_clock::time_point entered_at_;
    std::chrono::steady_clock::time_point expires_at_;
    std::chrono::milliseconds ttl_;
};

[[nodiscard]] result<finite_submission_window> make_finite_submission_window(
    std::chrono::steady_clock::time_point entered_at,
    std::chrono::milliseconds ttl) noexcept;

} // namespace inputnext::output
