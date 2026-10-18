#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include "metadata/provider.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/input_binding.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "../core/publication_source.hpp"

namespace inputnext {

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
    enum class phase : std::uint8_t { created, starting, running, stopping, drained, failed };
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

    struct owner_identity final {
        std::size_t slot_index{};
        std::uint64_t generation{};
    };

    struct facts_generation final {
        std::uint64_t value{};
    };

    struct owner_slot final {
        owner_phase state{owner_phase::free};
        gameinput_detail::native_device_handle device{};
        std::uint64_t generation{};
        bool retire_requested{};
        bool registry_retire_started{};
        bool registry_retired{};
        bool release_started{};
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
    [[nodiscard]] gameinput_stop_report stop_status_locked() const noexcept;

    device_registry& registry;
    gameinput_options options;
    std::unique_ptr<gameinput_detail::native_gameinput> native;
    std::unique_ptr<metadata_provider> metadata;
    mutable std::mutex table_gate;
    std::mutex catalogue_gate;
    std::mutex service_gate;
    std::condition_variable query_drained;
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
    std::optional<maintenance_time> next_maintenance;
};

} // namespace inputnext
