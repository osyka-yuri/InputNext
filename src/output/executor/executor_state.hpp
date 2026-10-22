#pragma once

#include "output/execution/engine.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace inputnext::output {

struct executor::implementation final {
    enum class worker_phase : std::uint8_t { free, starting, running, joining, joined_pending };

    struct worker_record final {
        detail::lane_token lane{};
        worker_phase phase{worker_phase::free};
        std::jthread thread;
    };

    class operation final {
    public:
        explicit operation(implementation& state) noexcept;
        ~operation() noexcept;

        operation(const operation&) = delete;
        operation& operator=(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(operation&&) = delete;

        [[nodiscard]] bool entered() const noexcept { return entered_; }

    private:
        implementation* state_{};
        bool entered_{};
    };

    implementation(authority& authority_owner, execution_owner attached_owner_value);
    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;
    ~implementation() noexcept = default;

    [[nodiscard]] execution_result<void> start_coordinator();
    [[nodiscard]] execution_result<backend_handle> register_backend(connection_target target,
        std::unique_ptr<native_sink>& candidate,
        channel_mask supported_channels,
        std::optional<backend_handle> expected);
    [[nodiscard]] execution_result<void> start_delivery_worker(
        const detail::registration_reservation& reservation);
    [[nodiscard]] execution_result<void> stop_and_join_worker(detail::lane_token lane) noexcept;
    [[nodiscard]] execution_result<void> join_worker_for_shutdown(
        std::size_t worker_index) noexcept;
    [[nodiscard]] execution_result<std::size_t> reserve_worker_slot(
        detail::lane_token lane) noexcept;
    void release_unstarted_worker_slot(std::size_t index, detail::lane_token lane) noexcept;
    [[nodiscard]] execution_result<void>
    finish_worker_join(std::size_t index, detail::lane_token lane, std::jthread thread) noexcept;
    void delivery_loop(detail::lane_token lane, const std::stop_token& stop_token) noexcept;
    void drain_after_wait_failure(detail::lane_token lane) const noexcept;
    void coordinator_loop(const std::stop_token& stop_token) noexcept;
    [[nodiscard]] bool refresh_current_backends() noexcept;
    [[nodiscard]] bool refresh_backend(backend_handle backend) noexcept;
    void close_admission() noexcept;
    void finish_operation() noexcept;
    void shutdown() noexcept;

    authority& owner;
    const execution_owner attached_owner;
    std::unique_ptr<detail::execution_engine> engine;

    mutable std::mutex lifecycle_gate;
    std::condition_variable lifecycle_changed;
    bool accepting{true};
    bool shutdown_started{};
    bool shutdown_complete{};
    bool engine_closed{};
    std::size_t active_operations{};
    std::jthread coordinator;
    std::array<worker_record, detail::execution_lane_capacity> workers{};
};

} // namespace inputnext::output
