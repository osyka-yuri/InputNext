#pragma once

#include "output/authority/execution.hpp"
#include "output/execution/engine.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>

namespace inputnext::test::output {

class execution_engine_scope final {
public:
    explicit execution_engine_scope(inputnext::output::authority& authority)
        : authority_(authority) {
        auto attached = inputnext::output::detail::authority_execution_access::attach(authority_);
        if (!attached.has_value()) {
            throw std::runtime_error{"Could not attach output execution owner"};
        }
        owner_ = *attached;
        try {
            engine_ =
                std::make_unique<inputnext::output::detail::execution_engine>(authority_, owner_);
        } catch (...) {
            const auto detached =
                inputnext::output::detail::authority_execution_access::detach(authority_, owner_);
            if (!detached.has_value()) {
                std::terminate();
            }
            throw;
        }
    }

    ~execution_engine_scope() noexcept {
        if (!engine_) {
            require_success(
                inputnext::output::detail::authority_execution_access::detach(authority_, owner_));
            return;
        }
        engine_->close_all();
        for (std::size_t index = 0U; index < lane_count_; ++index) {
            if (!lanes_[index].valid()) {
                continue;
            }
            for (std::size_t attempt = 0U;
                attempt < inputnext::output::detail::execution_backend_capacity + 1U &&
                engine_->drive_one(lanes_[index]);
                ++attempt) {
            }
        }
        for (std::size_t index = 0U; index < lane_count_; ++index) {
            if (!lanes_[index].valid()) {
                continue;
            }
            if (engine_->worker_should_exit(lanes_[index])) {
                require_success(worker_joined(lanes_[index]));
            }
        }
        engine_.reset();
        require_success(
            inputnext::output::detail::authority_execution_access::detach(authority_, owner_));
    }

    execution_engine_scope(const execution_engine_scope&) = delete;
    execution_engine_scope& operator=(const execution_engine_scope&) = delete;
    execution_engine_scope(execution_engine_scope&&) = delete;
    execution_engine_scope& operator=(execution_engine_scope&&) = delete;

    [[nodiscard]] inputnext::output::detail::execution_engine& engine() noexcept {
        return *engine_;
    }

    [[nodiscard]] inputnext::output::execution_owner owner() const noexcept { return owner_; }

    [[nodiscard]] inputnext::output::execution_result<inputnext::output::backend_handle>
    register_backend(const inputnext::connection_target target,
        std::unique_ptr<inputnext::output::native_sink>& candidate,
        const inputnext::output::channel_mask supported_channels,
        const std::optional<inputnext::output::backend_handle> expected = std::nullopt) {
        auto reservation =
            engine_->reserve_backend(target, candidate, supported_channels, expected);
        if (!reservation.has_value()) {
            return std::unexpected{reservation.error()};
        }
        remember(reservation->lane);
        if (reservation->start_worker) {
            const auto started = engine_->worker_started(reservation->lane);
            if (!started.has_value()) {
                const auto rolled_back = worker_start_failed(reservation->lane);
                if (!rolled_back.has_value()) {
                    return std::unexpected{rolled_back.error()};
                }
                return std::unexpected{started.error()};
            }
        }
        auto committed = engine_->commit_backend(*reservation, candidate);
        if (!committed.has_value()) {
            const auto canceled = engine_->cancel_backend(*reservation);
            if (!canceled.has_value()) {
                return std::unexpected{canceled.error()};
            }
        }
        return committed;
    }

    void remember(inputnext::output::detail::lane_token lane) noexcept {
        for (std::size_t index = 0U; index < lane_count_; ++index) {
            if (lanes_[index] == lane) {
                return;
            }
            if (!lanes_[index].valid()) {
                lanes_[index] = lane;
                return;
            }
        }
        if (lane_count_ < lanes_.size()) {
            lanes_[lane_count_++] = lane;
        }
    }

    [[nodiscard]] inputnext::output::execution_result<void> worker_joined(
        const inputnext::output::detail::lane_token lane) noexcept {
        auto result = engine_->worker_joined(lane);
        if (result.has_value()) {
            forget_lane(lane);
        }
        return result;
    }

    [[nodiscard]] inputnext::output::execution_result<void> worker_start_failed(
        const inputnext::output::detail::lane_token lane) noexcept {
        auto result = engine_->worker_start_failed(lane);
        if (result.has_value()) {
            forget_lane(lane);
        }
        return result;
    }

private:
    void forget_lane(const inputnext::output::detail::lane_token lane) noexcept {
        for (std::size_t index = 0U; index < lane_count_; ++index) {
            if (lanes_[index] == lane) {
                lanes_[index] = {};
                break;
            }
        }
    }

    static void require_success(const inputnext::output::execution_result<void> result) noexcept {
        if (!result.has_value()) {
            std::terminate();
        }
    }

    inputnext::output::authority& authority_;
    inputnext::output::execution_owner owner_{};
    std::unique_ptr<inputnext::output::detail::execution_engine> engine_;
    std::array<inputnext::output::detail::lane_token,
        inputnext::output::detail::execution_lane_capacity>
        lanes_{};
    std::size_t lane_count_{};
};

} // namespace inputnext::test::output
