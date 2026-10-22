#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "manager.hpp"
#include "schedule.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace inputnext::gameinput_detail {

struct gameinput_output_connections::implementation final {
    using clock = std::chrono::steady_clock;

    static constexpr std::size_t desired_capacity = gameinput_live_connection_capacity;
    static constexpr std::size_t resident_capacity = 128U;

    enum class backend_state : std::uint8_t {
        free,
        creating,
        registering,
        current,
        retiring,
        terminal_unresolved,
    };

    enum class backend_scan_step : std::uint8_t {
        progress_ready,
        skip_backend,
        stop_scan,
    };

    struct desired_record final {
        bool occupied{};
        connection_target target{};
        bool description_known{};
        std::uint64_t catalogue_generation{};
        std::optional<rumble_capabilities> capabilities{std::nullopt};
        clock::time_point next_description;
        std::uint8_t retry_level{};
        bool retry_armed{};
        clock::time_point retry_due;
        bool retry_blocked{};
        bool resource_waiting{};
        clock::time_point resource_due;
    };

    struct backend_record final {
        backend_state state{backend_state::free};
        connection_target target{};
        std::optional<output::backend_handle> handle{std::nullopt};
        std::optional<output::retirement_receipt> receipt{std::nullopt};
        std::optional<output::backend_progress> cached_progress{std::nullopt};
    };

    implementation(runtime_output_access& runtime_access,
        output::executor& output_executor) noexcept;
    ~implementation() noexcept;

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    void worker_loop() noexcept;
    [[nodiscard]] bool worker_iteration();
    void handle_worker_exception() noexcept;
    [[nodiscard]] bool worker_work_ready_locked() const noexcept;
    [[nodiscard]] bool worker_can_finish_locked() const noexcept;
    void run_cycle() noexcept;
    void scan_backends() noexcept;
    [[nodiscard]] bool process_backend(std::size_t slot, output::backend_handle handle) noexcept;
    [[nodiscard]] backend_scan_step query_backend_progress(output::backend_handle handle,
        output::backend_progress& progress) noexcept;
    [[nodiscard]] backend_scan_step cache_backend_progress(std::size_t slot,
        output::backend_handle handle,
        output::backend_progress progress,
        std::optional<output::retirement_receipt>& receipt,
        bool& retire) noexcept;
    [[nodiscard]] backend_scan_step request_backend_retirement(std::size_t slot,
        output::backend_handle handle,
        output::backend_progress progress,
        std::optional<output::retirement_receipt>& receipt,
        bool should_retire) noexcept;
    [[nodiscard]] backend_scan_step acknowledge_drained_backend(std::size_t slot,
        output::backend_handle handle,
        output::backend_progress progress,
        std::optional<output::retirement_receipt> receipt) noexcept;
    void scan_descriptions() noexcept;
    [[nodiscard]] std::size_t snapshot_descriptions(
        std::array<connection_target, desired_capacity>& targets) noexcept;
    [[nodiscard]] bool describe_target(connection_target target) noexcept;
    void attempt_one_creation() noexcept;
    void process_one_creation(std::size_t slot, connection_target target) noexcept;
    void process_reserved_candidate(std::size_t slot,
        connection_target target,
        runtime_output_lease lease) noexcept;
    void handle_reserve_failure(std::size_t slot,
        connection_target target,
        gameinput_failure failure) noexcept;
    [[nodiscard]] static native_result<std::unique_ptr<output::native_sink>>
    make_candidate(runtime_output_lease lease, bool& threw) noexcept;
    [[nodiscard]] bool candidate_may_proceed(connection_target target) noexcept;
    [[nodiscard]] bool mark_candidate_registering(std::size_t slot,
        connection_target target) noexcept;
    void cancel_candidate(std::size_t slot, connection_target target) noexcept;
    void handle_candidate_failure(std::size_t slot,
        connection_target target,
        native_failure failure,
        bool threw) noexcept;
    void register_candidate(std::size_t slot,
        connection_target target,
        std::unique_ptr<output::native_sink> candidate) noexcept;
    void retain_unresolved_candidate(std::size_t slot,
        connection_target target,
        std::optional<output::backend_handle> handle) noexcept;
    void publish_registered_candidate(std::size_t slot,
        connection_target target,
        output::backend_handle handle) noexcept;
    void handle_registration_failure(std::size_t slot,
        connection_target target,
        std::unique_ptr<output::native_sink> candidate,
        output::execution_error failure) noexcept;
    void schedule_after_cycle() noexcept;
    [[nodiscard]] std::optional<clock::time_point> next_deadline_locked() const noexcept;
    [[nodiscard]] bool any_backend_locked() const noexcept;
    [[nodiscard]] connection_schedule::creation_state creation_state_locked(
        const desired_record& entry) const noexcept;
    [[nodiscard]] bool creation_allowed_locked(const desired_record& entry,
        clock::time_point now) const noexcept;
    [[nodiscard]] bool has_same_device_predecessor_locked(connection_target target) const noexcept;
    [[nodiscard]] desired_record* find_desired_locked(connection_target target) noexcept;
    [[nodiscard]] const desired_record* find_desired_locked(
        connection_target target) const noexcept;
    [[nodiscard]] std::size_t find_free_backend_locked() const noexcept;
    [[nodiscard]] connection_control_report report_locked() const noexcept;
    void fail_locked(connection_control_failure failure) noexcept;
    void fallback_locked(connection_control_failure failure) noexcept;
    static void arm_retry_locked(desired_record& entry, clock::time_point completed) noexcept;
    static void reset_retry_locked(desired_record& entry) noexcept;
    void complete_candidate_locked(std::size_t slot) noexcept;
    void finish_worker_locked() noexcept;
    void publish_backend_progress_locked(std::size_t slot, output::backend_progress value) noexcept;
    [[nodiscard]] bool should_retire_locked(connection_target target) const noexcept;

    runtime_output_access& runtime;
    output::executor& executor;

    mutable std::mutex gate;
    std::condition_variable changed;
    std::mutex shutdown_gate;
    std::thread worker;

    connection_control_phase phase{connection_control_phase::accepting};
    std::array<desired_record, desired_capacity> desired{};
    std::array<backend_record, resident_capacity> backends{};
    std::size_t candidate_obligations{};
    std::size_t creation_handoffs{};
    std::size_t backend_obligations{};
    std::size_t pending_retirements{};
    bool admission_quiescent{true};
    bool control_finished{};
    bool control_joined{};
    bool all_retirements_acknowledged{true};
    bool executor_shutdown_returned{};
    bool inventory_dirty{true};
    bool admission_dirty{};
    bool stop_dirty{};
    bool progress_dirty{true};
    bool fallback{};
    bool admissions_frozen{};
    bool runtime_admission_closed{};
    std::optional<clock::time_point> next_progress_scan{clock::time_point::min()};
    std::optional<connection_control_failure> first_failure{std::nullopt};
};

} // namespace inputnext::gameinput_detail
