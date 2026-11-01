#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace inputnext::gameinput_detail {

enum class manual_step_result : std::uint8_t { passed, failed };

using manual_confirmation_id = std::uint64_t;

struct manual_step_confirmation final {
    manual_step_result result{};
    manual_confirmation_id readiness_id{};
    std::chrono::steady_clock::time_point accepted_at;
};

class manual_sequence final {
public:
    using clock = std::chrono::steady_clock;

    explicit manual_sequence(const std::size_t step_count, const std::size_t first_step = 0U)
        : step_count_(step_count),
          first_step_(first_step),
          index_(first_step) {
        if (first_step > step_count) {
            throw std::invalid_argument("The first step exceeds the sequence length.");
        }
    }

    [[nodiscard]] std::size_t index() const noexcept { return index_; }

    [[nodiscard]] std::size_t failed() const noexcept { return failed_; }

    [[nodiscard]] std::size_t passed() const noexcept { return index_ - first_step_ - failed_; }

    [[nodiscard]] bool complete() const noexcept { return index_ == step_count_; }

    [[nodiscard]] bool awaiting_confirmation(
        const clock::time_point now = clock::now()) const noexcept {
        return readiness_.has_value() && now < readiness_->until && !complete();
    }

    [[nodiscard]] std::optional<manual_confirmation_id> mark_ready(
        const clock::time_point until = clock::time_point::max(),
        const clock::time_point since = clock::now()) noexcept {
        if (complete() || since >= until) {
            mark_pending();
            return std::nullopt;
        }

        if (readiness_.has_value() && readiness_->until == until) {
            return readiness_->id;
        }

        if (next_readiness_id_ == std::numeric_limits<manual_confirmation_id>::max()) {
            mark_pending();
            return std::nullopt;
        }

        const auto id = ++next_readiness_id_;
        readiness_ = readiness_window{.id = id, .since = since, .until = until};
        return id;
    }

    void mark_pending() noexcept { readiness_.reset(); }

    [[nodiscard]] bool record_result(const manual_step_confirmation& confirmation,
        const clock::time_point processed_at) noexcept {
        if (!readiness_.has_value() || confirmation.readiness_id != readiness_->id ||
            confirmation.accepted_at < readiness_->since ||
            confirmation.accepted_at > processed_at) {
            return false;
        }

        if (confirmation.accepted_at >= readiness_->until) {
            mark_pending();
            return false;
        }

        failed_ += confirmation.result == manual_step_result::failed ? 1U : 0U;
        ++index_;
        mark_pending();
        return true;
    }

    [[nodiscard]] bool record_result(const manual_step_confirmation& confirmation) noexcept {
        return record_result(confirmation, clock::now());
    }

private:
    struct readiness_window final {
        manual_confirmation_id id{};
        clock::time_point since;
        clock::time_point until;
    };

    const std::size_t step_count_;
    const std::size_t first_step_;
    std::size_t index_{};
    std::size_t failed_{};
    manual_confirmation_id next_readiness_id_{};
    std::optional<readiness_window> readiness_;
};

} // namespace inputnext::gameinput_detail
