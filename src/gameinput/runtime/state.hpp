#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "history/cursor.hpp"
#include "history/store.hpp"
#include "metadata/provider.hpp"
#include "owner_identity.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace inputnext {

namespace gameinput_detail {
class publication_source;
class source_history_cursor;
struct seed_ticket;
} // namespace gameinput_detail

namespace runtime_detail {
inline constexpr std::size_t native_owner_capacity = 128U;
inline constexpr auto maintenance_period = std::chrono::milliseconds{4};
inline constexpr auto retry_period = std::chrono::seconds{1};

[[nodiscard]] inline bool is_device_disconnected(
    const gameinput_detail::native_failure& failure) noexcept {
    return failure.cause == gameinput_detail::native_failure_cause::device_disconnected;
}
} // namespace runtime_detail

struct gameinput_runtime::implementation final {
    struct history_open_hook_snapshot final {
        void (*hook)(void*) noexcept {};
        void* context{};
    };

    enum class phase : std::uint8_t { created, starting, running, stopping, drained, failed };
    enum class output_cleanup_work : std::uint8_t { release_device, release_native_api };
    enum class owner_phase : std::uint8_t {
        free,
        observed,
        admitting,
        active,
        retiring,
        releasing
    };
    enum class attachment_status : std::uint8_t { stale, active, retiring };

    struct reading_owner final {
        implementation* runtime{};
        std::size_t slot_index{};
        std::uint64_t generation{};
    };

    using owner_identity = gameinput_detail::runtime_owner_identity;

    struct facts_generation final {
        std::uint64_t value{};
    };

    struct owner_slot final {
        owner_phase state{owner_phase::free};
        gameinput_detail::native_device_handle device{};
        std::uint64_t generation{};
        std::size_t history_leases{};
        bool retire_requested{};
        bool registry_retire_started{};
        bool registry_retired{};
        bool release_started{};
        bool release_inflight{};
        std::size_t output_leases{};
        connection_target target{};
        gameinput_detail::publication_source* source{};
        read_binding reader{};
        read_pin residency;
        reading_owner reading_owner_context{};
        gameinput_detail::native_reading_callback_context reading_context{};
        gameinput_detail::native_callback_token reading_token{};
        bool reading_registration_inflight{};
        bool registration_failed{};
        std::optional<maintenance_time> registration_retry_at;
        std::optional<maintenance_time> reading_drain_retry_at;
        std::optional<maintenance_time> poll_at;
        std::uint64_t facts_requested{1U};
        std::uint64_t facts_installed{};
        bool metadata_fault{};
        std::optional<maintenance_time> facts_retry_at;
        std::optional<maintenance_time> admission_retry_at;
    };

    struct catalogue_entry final {
        bool active{};
        connection_target target{};
        std::uint64_t slot_generation{};
        std::uint64_t request_generation{};
        std::uint64_t catalogue_generation{};
        native_device_facts facts{};
    };

    struct history_provider final : history_detail::history_provider {
        explicit history_provider(implementation& runtime) noexcept;

        [[nodiscard]] history_result<std::unique_ptr<history_detail::history_cursor>> open_cursor(
            connection_target target) override;
        [[nodiscard]] bool is_live(connection_target target) noexcept override;

        implementation& runtime;
    };

    struct history_lease final {
        history_lease(implementation& runtime, owner_identity owner, read_pin residency) noexcept;
        ~history_lease();

        history_lease(const history_lease&) = delete;
        history_lease& operator=(const history_lease&) = delete;
        history_lease(history_lease&& other) noexcept;
        history_lease& operator=(history_lease&& other) noexcept;

        void release() noexcept;

        implementation* runtime{};
        owner_identity owner{};
        read_pin residency;
    };

    class leased_history_cursor final : public history_detail::history_cursor {
    public:
        leased_history_cursor(history_lease lease,
            std::unique_ptr<gameinput_detail::source_history_cursor> cursor) noexcept;
        ~leased_history_cursor() override;

        leased_history_cursor(const leased_history_cursor&) = delete;
        leased_history_cursor& operator=(const leased_history_cursor&) = delete;
        leased_history_cursor(leased_history_cursor&&) = delete;
        leased_history_cursor& operator=(leased_history_cursor&&) = delete;

        [[nodiscard]] history_result<history_detail::cursor_seed> start(
            history_time now) noexcept override;
        [[nodiscard]] history_detail::cursor_step next(history_time now) noexcept override;

    private:
        history_lease lease_;
        std::unique_ptr<gameinput_detail::source_history_cursor> cursor_;
    };

    implementation(device_registry& device_registry,
        gameinput_options runtime_options,
        std::unique_ptr<gameinput_detail::native_gameinput> native_port,
        std::unique_ptr<metadata_provider> metadata_port);

    ~implementation() = default;
    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;
    [[nodiscard]] gameinput_result<void> start(maintenance_time now);
    void maintain(maintenance_time now);
    [[nodiscard]] std::optional<maintenance_time> next_deadline() const;
    [[nodiscard]] connection_inventory connections() const;
    [[nodiscard]] metadata_result<metadata_snapshot> query_metadata(connection_target target);
    [[nodiscard]] history_result<history_reader_handle> open_history(connection_target target,
        history_time now,
        std::optional<std::uint32_t> reserved_position = std::nullopt);
    [[nodiscard]] history_batch read_history(history_reader_handle reader, history_time now);
    [[nodiscard]] history_result<void> close_history(history_reader_handle reader);
    [[nodiscard]] std::optional<history_failure> begin_history_call(bool allow_stopping,
        history_open_hook_snapshot* hook_snapshot = nullptr) noexcept;
    void end_history_call() noexcept;
    void release_history_lease(owner_identity owner) noexcept;
    [[nodiscard]] gameinput_stop_report stop(maintenance_time now);
    void shutdown() noexcept;
    [[nodiscard]] owner_slot* find_live_owner(connection_target target) noexcept;
    [[nodiscard]] const owner_slot* find_live_owner(connection_target target) const noexcept;
    [[nodiscard]] const catalogue_entry* find_catalogue(connection_target target,
        std::uint64_t slot_generation) const noexcept;
    [[nodiscard]] static bool polling_required_locked(const owner_slot& owner);
    [[nodiscard]] std::optional<maintenance_time> next_rescan_deadline_locked() const;
    [[nodiscard]] static std::optional<maintenance_time>
    next_owner_deadline_locked(const owner_slot& owner, bool admission_capacity_available);
    void request_rescan_locked() noexcept;
    static void request_facts_locked(owner_slot& owner) noexcept;
    static void device_event(void* context,
        gameinput_detail::native_device_handle device,
        gameinput_detail::native_device_event event) noexcept;
    void on_device_event(gameinput_detail::native_device_handle device,
        gameinput_detail::native_device_event event) noexcept;
    static void reading_event(void* context, const gameinput_detail::reading_parts& parts) noexcept;
    void on_reading_event(owner_identity identity,
        const gameinput_detail::reading_parts& parts) noexcept;
    void service_retirements(maintenance_time now);
    void service_reading_drains(maintenance_time now);
    void service_admissions(maintenance_time now);
    void service_readings(maintenance_time now);
    void try_register_readings(std::size_t index, std::uint64_t generation, maintenance_time now);
    void seed_current(std::size_t index,
        std::uint64_t generation,
        gameinput_detail::seed_ticket seed,
        maintenance_time now);
    void fetch_seed_lane(std::size_t index,
        std::uint64_t generation,
        gameinput_detail::native_device_handle device,
        gameinput_detail::native_reading_lane lane,
        gameinput_detail::seed_ticket seed);
    void poll_current(std::size_t index, std::uint64_t generation, maintenance_time now);

    void service_facts_refresh(maintenance_time now);
    [[nodiscard]] std::optional<std::uint64_t> install_catalogue(connection_target target,
        std::uint64_t slot_generation,
        std::uint64_t request_generation,
        native_device_facts facts);
    [[nodiscard]] std::optional<std::uint64_t> install_catalogue_for_request(
        owner_identity identity,
        facts_generation request_generation,
        native_device_facts facts);
    void acknowledge_facts(owner_identity identity,
        facts_generation request_generation,
        std::optional<std::uint64_t> installed);
    void erase_catalogue(connection_target target, std::uint64_t slot_generation) noexcept;
    void mark_retiring(std::size_t index, std::uint64_t generation) noexcept;
    void service_rescan(maintenance_time now);
    void service_rescan_stop(maintenance_time now);
    void service_watch_stop(maintenance_time now);
    void service_releasable_owners();
    void output_cleanup_loop() noexcept;
    [[nodiscard]] output_cleanup_work wait_for_output_cleanup_work(owner_identity& identity,
        gameinput_detail::native_device_handle& device,
        read_pin& residency,
        std::unique_ptr<gameinput_detail::native_gameinput>& final_native);
    void finish_output_device_release(owner_identity identity,
        gameinput_detail::native_device_handle device,
        read_pin residency) noexcept;
    void finish_output_native_cleanup(
        std::unique_ptr<gameinput_detail::native_gameinput> final_native) noexcept;
    void notify_output_cleanup() noexcept;
    void release_output_lease(owner_identity identity, read_pin residency) noexcept;
    void retry_admission(owner_identity identity, maintenance_time now) noexcept;
    void process_admission(owner_identity identity,
        gameinput_detail::native_device_handle device,
        std::uint64_t facts_request,
        maintenance_time now);
    [[nodiscard]] attachment_status attach_admitted_owner(owner_identity identity,
        connection_target target,
        gameinput_detail::publication_source* source,
        read_binding reader,
        read_pin& residency,
        maintenance_time now);
    void retire_failed_admission(owner_identity identity,
        connection_target target,
        gameinput_detail::publication_source* source,
        read_pin* residency);
    [[nodiscard]] bool retire_registry_target(connection_target target);
    [[nodiscard]] bool all_native_owners_drained_locked() const noexcept;
    [[nodiscard]] gameinput_stop_report stop_status_locked(
        std::size_t resident_history_banks) const noexcept;

    device_registry& registry;
    gameinput_options options;
    std::unique_ptr<gameinput_detail::native_gameinput> native;
    std::unique_ptr<metadata_provider> metadata;
    history_provider history_provider_port;
    history_detail::history_store history;
    mutable std::mutex table_gate;
    std::mutex catalogue_gate;
    std::mutex service_gate;
    std::condition_variable query_drained;
    std::condition_variable output_cleanup_wake;
    phase state{phase::created};
    std::array<owner_slot, runtime_detail::native_owner_capacity> owners{};
    std::array<bool, runtime_detail::native_owner_capacity> facts_attempted{};
    std::array<catalogue_entry, gameinput_live_connection_capacity> catalogue{};
    gameinput_detail::native_device_callback_context watch_context{};
    gameinput_detail::native_device_callback_context rescan_context{};
    gameinput_detail::native_callback_token watch_token{};
    gameinput_detail::native_callback_token rescan_token{};
    bool watch_registration_inflight{};
    bool rescan_registration_inflight{};
    std::optional<maintenance_time> watch_drain_retry_at;
    std::optional<maintenance_time> rescan_drain_retry_at;
    std::optional<maintenance_time> rescan_registration_retry_at;
    std::uint64_t next_owner_generation{1U};
    bool owner_generation_exhausted{};
    std::uint64_t rescan_requested{};
    std::uint64_t rescan_acknowledged{};
    bool rescan_exhausted{};
    std::uint64_t catalogue_generation{};
    std::size_t active_queries{};
    std::size_t active_history_calls{};
    bool output_enabled{};
    bool output_admission_open{};
    bool native_cleanup_requested{};
    bool native_cleanup_started{};
    bool native_cleanup_finished{};
    std::thread output_cleanup_worker;
    void (*before_history_open_test_hook)(void*) noexcept {};
    void* before_history_open_test_context{};
    std::optional<maintenance_time> next_maintenance;
};

} // namespace inputnext
