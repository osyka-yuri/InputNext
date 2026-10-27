#pragma once

#include "engine.hpp"
#include "output/authority/execution.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {

struct execution_engine::implementation final {
    enum class lane_phase : std::uint8_t { free, starting, running, closing };

    struct lane_record final {
        lane_phase phase{lane_phase::free};
        lane_token token{};
        runtime_id runtime{};
        device_key device{};
        std::mutex physical_fence;
        std::condition_variable_any changed;
        bool shutdown_requested{};
    };

    enum class backend_state : std::uint8_t {
        free,
        reserved,
        current,
        retiring,
        cleaning,
        drained
    };

    struct backend_record final {
        backend_state state{backend_state::free};
        backend_id id{};
        lane_token lane{};
        connection_target reserved_target{};
        std::optional<backend_handle> expected_backend;
        backend_handle handle{};
        channel_mask supported_channels{};
        body_route_support route_support{body_route_support::classic_only};
        channel_mask unsupported_channels{};
        std::unique_ptr<native_sink> sink;
        std::optional<represented_projection> projection;
        std::uint64_t observed_logical_version{};
        publication_id publication{};
        std::uint64_t next_publication_id{1U};
        std::uint64_t next_attempt_id{1U};
        delivery_state delivery{delivery_state::pending};
        bool delivery_needed{};
        bool write_in_flight{};
        bool retry_requested{};
        bool refresh_requested{};
        std::uint32_t consecutive_retryable_failures{};
        clock::time_point due;
        std::optional<attempt_id> last_attempt;
        std::optional<completed_attempt> last_completed_attempt;
        std::optional<body_route_ack> acknowledged_body_route;
        std::optional<retirement_receipt> receipt;
    };

    enum class drive_action_kind : std::uint8_t { idle, progressed, cleanup, write };

    struct drive_action final {
        drive_action_kind kind{drive_action_kind::idle};
        backend_handle backend{};
        native_command command{};
        native_sink* sink{};
        std::unique_ptr<native_sink> cleanup_sink = nullptr;
        std::atomic<bool>* render_busy{};
    };

    struct lane_selection final {
        std::uint16_t slot{};
        bool start_worker{};
    };

    implementation(authority& authority_owner, execution_owner owner);

    [[nodiscard]] lane_record* find_lane(lane_token token) noexcept;
    [[nodiscard]] const lane_record* find_lane(lane_token token) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_lane(runtime_id runtime,
        device_key device) const noexcept;
    [[nodiscard]] backend_record* find_backend(backend_handle handle) noexcept;
    [[nodiscard]] const backend_record* find_backend(backend_handle handle) const noexcept;
    [[nodiscard]] backend_record* find_reserved(backend_id id, lane_token lane) noexcept;
    [[nodiscard]] backend_record* find_current(lane_token lane) noexcept;
    [[nodiscard]] const backend_record* find_current(lane_token lane) const noexcept;
    [[nodiscard]] static backend_progress make_progress(const backend_record& backend) noexcept;
    [[nodiscard]] backend_record* find_retiring(lane_token lane) noexcept;
    [[nodiscard]] bool lane_has_records(lane_token lane) const noexcept;
    [[nodiscard]] bool lane_has_reservations(lane_token lane) const noexcept;
    [[nodiscard]] std::size_t current_lane_count() const noexcept;
    [[nodiscard]] std::size_t current_backend_count() const noexcept;
    [[nodiscard]] bool lane_has_pending_cleanup(lane_token lane) const noexcept;
    [[nodiscard]] bool lane_should_exit(lane_token token) const noexcept;
    [[nodiscard]] bool lane_has_due_work(lane_token lane, clock::time_point now) const noexcept;
    [[nodiscard]] std::optional<clock::time_point> next_due(lane_token lane) const noexcept;
    void notify_lane(lane_token token) noexcept;
    void notify_coordinator_refresh() noexcept;
    void retire_locked(backend_record& backend, retirement_reason reason) noexcept;
    void maybe_close_lane_locked(lane_token token) noexcept;
    [[nodiscard]] execution_result<const backend_record*> validate_expected_backend(
        connection_target target,
        std::optional<backend_handle> expected) const noexcept;
    [[nodiscard]] execution_result<lane_selection> select_lane(connection_target target,
        bool replacing,
        const backend_record* expected_backend) noexcept;
    [[nodiscard]] execution_result<registration_reservation> reserve_selected_lane(
        connection_target target,
        channel_mask supported_channels,
        body_route_support route_support,
        std::optional<backend_handle> expected,
        lane_selection selection) noexcept;
    [[nodiscard]] drive_action prepare_drive(lane_token token) noexcept;
    void finish_cleanup(lane_token token, backend_handle handle) noexcept;
    void finish_attempt(const native_command& command, native_result result) noexcept;
    static void retain_body_ack_for(backend_record& backend,
        const std::optional<body_route_intent>& route) noexcept;
    [[nodiscard]] execution_result<completion_disposition> complete_body_route(
        backend_record& backend,
        const native_command& command,
        native_result result) noexcept;
    void apply_native_result(backend_record& backend, native_result result) noexcept;
    [[nodiscard]] bool apply_qualification_result(backend_record& backend,
        const execution_result<bool>& result) noexcept;

    authority& owner;
    const execution_owner execution;
    mutable std::mutex gate;
    std::condition_variable_any refresh_changed;
    std::array<lane_record, execution_lane_capacity> lanes{};
    std::array<backend_record, execution_backend_capacity> backends{};
    std::array<std::atomic<bool>, execution_backend_capacity> render_busy{};
    std::uint64_t next_lane_generation{1U};
    std::uint64_t next_backend_id{1U};
    bool closed{};
    bool coordinator_refresh_requested{};
};

} // namespace inputnext::output::detail
