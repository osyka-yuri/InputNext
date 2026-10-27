#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_observation.hpp>

#include "output/authority/admitted_body_render.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

namespace inputnext::output::detail {

struct execution_engine_test_access;

inline constexpr std::size_t execution_lane_capacity = 128U;
inline constexpr std::size_t execution_backend_capacity = 128U;
inline constexpr std::size_t execution_current_lane_capacity = 16U;

struct current_backend_batch final {
    std::array<backend_handle, execution_current_lane_capacity> handles{};
    std::size_t count{};
};

struct lane_token final {
    std::uint16_t slot{};
    std::uint64_t generation{};

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0U; }

    friend constexpr bool operator==(lane_token, lane_token) noexcept = default;
};

struct registration_reservation final {
    backend_id reserved_backend{};
    lane_token lane{};
    connection_target target{};
    channel_mask supported_channels{};
    std::optional<backend_handle> expected_backend;
    bool start_worker{};
    body_route_support route_support{body_route_support::classic_only};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return reserved_backend.valid() && lane.valid();
    }
};

class execution_engine final {
public:
    using clock = std::chrono::steady_clock;

    execution_engine(authority& owner, execution_owner attached_owner);
    ~execution_engine() noexcept;

    execution_engine(const execution_engine&) = delete;
    execution_engine& operator=(const execution_engine&) = delete;
    execution_engine(execution_engine&&) = delete;
    execution_engine& operator=(execution_engine&&) = delete;

    [[nodiscard]] execution_result<registration_reservation> reserve_backend(
        connection_target target,
        const std::unique_ptr<native_sink>& candidate,
        channel_mask supported_channels,
        std::optional<backend_handle> expected = std::nullopt,
        body_route_support route_support = body_route_support::classic_only);
    [[nodiscard]] execution_result<void> worker_started(lane_token token) noexcept;
    [[nodiscard]] execution_result<void> worker_start_failed(lane_token token) noexcept;
    [[nodiscard]] execution_result<backend_handle> commit_backend(
        const registration_reservation& reservation,
        std::unique_ptr<native_sink>& candidate);
    [[nodiscard]] execution_result<void> cancel_backend(
        const registration_reservation& reservation) noexcept;

    [[nodiscard]] execution_result<lane_token> lane_for(backend_handle handle) const noexcept;
    [[nodiscard]] execution_result<void> publish(backend_handle handle,
        const logical_snapshot& snapshot) noexcept;
    [[nodiscard]] execution_result<void> request_retry(backend_handle handle) noexcept;

    [[nodiscard]] bool wait_for_work(lane_token token, const std::stop_token& stop_token);
    [[nodiscard]] bool drive_one(lane_token token) noexcept;

    [[nodiscard]] bool wait_for_refresh(const std::stop_token& stop_token);
    [[nodiscard]] execution_result<current_backend_batch> current_backends() const noexcept;
    [[nodiscard]] bool has_current_backends() const noexcept;
    void request_drive() noexcept;

    [[nodiscard]] execution_result<retirement_receipt> retire(backend_handle handle,
        retirement_reason reason = retirement_reason::requested) noexcept;
    [[nodiscard]] execution_result<backend_progress> progress(backend_handle handle) const noexcept;
    [[nodiscard]] execution_result<backend_observation> observe(backend_handle handle) const;
    [[nodiscard]] execution_result<bool> qualifies_route(
        const body_route_ack& acknowledgement) const noexcept;
    [[nodiscard]] execution_result<std::optional<body_render_permit>> admit_body_render(
        const body_route_ack& acknowledgement,
        body_playback_identity expected_playback,
        std::size_t requested_frames) noexcept;
    [[nodiscard]] execution_result<std::optional<admitted_body_render>> admit_current_body_render(
        backend_handle expected_backend,
        audio_instance expected_audio,
        std::size_t requested_frames) noexcept;
    [[nodiscard]] execution_result<void> acknowledge_retirement(
        const retirement_receipt& receipt) noexcept;
    void close_all() noexcept;
    [[nodiscard]] bool worker_should_exit(lane_token token) const noexcept;
    [[nodiscard]] execution_result<void> worker_joined(lane_token token) noexcept;

private:
    friend struct execution_engine_test_access;
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::output::detail
