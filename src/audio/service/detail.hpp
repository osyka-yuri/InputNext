#pragma once

#include "audio/audio_service.hpp"
#include "audio/control_notification.hpp"
#include "audio/native_port.hpp"
#include "audio/native_types.hpp"

#include "output/body/audio_route_control.hpp"
#include "output/render/control.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace inputnext::audio_detail {

inline constexpr std::size_t audio_resident_capacity = 16U;
inline constexpr std::uint32_t audio_max_block_frames = 4800U;
inline constexpr std::uint32_t audio_max_ring_frames = 4800U;
inline constexpr std::chrono::milliseconds audio_control_reconcile_interval{4};
inline constexpr std::chrono::milliseconds audio_offer_lifetime{20};
inline constexpr std::chrono::milliseconds audio_native_event_timeout{250};
inline constexpr std::chrono::milliseconds audio_identity_refresh_interval{64};
inline constexpr std::chrono::milliseconds audio_identity_retry_interval{16};
inline constexpr std::chrono::milliseconds audio_identity_validity{256};

[[nodiscard]] inline std::int64_t time_ticks(
    const std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

inline constexpr std::array<std::chrono::milliseconds, 5U> audio_retry_delays{
    std::chrono::milliseconds{64},
    std::chrono::milliseconds{128},
    std::chrono::milliseconds{256},
    std::chrono::milliseconds{512},
    std::chrono::milliseconds{1000},
};

struct native_resource_key final {
    fixed_haptic_endpoint_id endpoint;
    native_guid container{};

    friend constexpr bool operator==(const native_resource_key&,
        const native_resource_key&) noexcept = default;
};

[[nodiscard]] inline bool native_guid_is_zero(const native_guid& value) noexcept {
    return std::ranges::all_of(value.bytes,
        [](const std::byte byte) { return byte == std::byte{}; });
}

[[nodiscard]] inline bool native_resource_conflicts(const native_resource_key& left,
    const native_resource_key& right) noexcept {
    return left.endpoint == right.endpoint || left.container == right.container;
}

[[nodiscard]] inline native_audio_format canonical_audio_format() noexcept {
    return {.encoding = native_sample_encoding::ieee_float,
        .channels = 4U,
        .sample_rate = 48000U,
        .bits_per_sample = 32U,
        .valid_bits_per_sample = 32U,
        .block_align = 16U,
        .average_bytes_per_second = 768000U,
        .channel_mask = 0x33U,
        .extension_size = 22U,
        .extensible = true};
}

enum class slot_lifecycle : std::uint8_t {
    vacant,
    opening,
    prepared,
    live,
    revoking,
    draining,
    retained,
};

enum class setup_phase : std::uint8_t {
    empty,
    candidate_ready,
    granted,
    rejected,
    prepared,
};

enum class handoff_phase : std::uint8_t {
    idle,
    writing_request,
    requested,
    admitting,
    offered,
    reclaiming,
    claimed,
    cancelled,
    completed,
};

struct render_request final {
    std::uint64_t slot_generation{};
    std::uint64_t sequence{};
    output::audio_instance audio{};
    std::uint32_t frame_bound{};
};

struct render_response final {
    std::uint64_t slot_generation{};
    std::uint64_t sequence{};
    output::audio_instance audio{};
    std::chrono::steady_clock::time_point expires_at;
    std::optional<output::detail::admitted_body_render> admitted = std::nullopt;
};

struct saturating_counter final {
    std::atomic<std::uint64_t> value{0U};
    std::atomic<bool>* saturation{};

    void add(const std::uint64_t amount) noexcept {
        auto previous = value.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = amount > UINT64_MAX - previous ? UINT64_MAX : previous + amount;
            if (value.compare_exchange_weak(previous,
                    next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                if (next == UINT64_MAX && amount > UINT64_MAX - previous && saturation != nullptr) {
                    saturation->store(true, std::memory_order_relaxed);
                }
                return;
            }
        }
    }
};

struct audio_slot final {
    std::atomic<slot_lifecycle> lifecycle{slot_lifecycle::vacant};
    std::atomic<bool> transport_closed{true};
    std::atomic<bool> worker_done;
    std::atomic<bool> native_cleanup_complete;
    std::atomic<bool> retirement_settled;
    std::atomic<bool> native_failed;
    std::atomic<bool> native_setup_started;
    std::atomic<bool> native_stream_started;
    std::atomic<bool> ready;
    std::atomic<bool> mmcss_registered;
    std::atomic<bool> counters_saturated;
    std::atomic<std::uint64_t> last_native_error_value;
    std::atomic<bool> has_native_error;
    std::atomic<std::uint8_t> last_metadata_error;
    std::atomic<bool> has_metadata_error;
    saturating_counter admitted_blocks{.saturation = &counters_saturated};
    saturating_counter source_frames_consumed{.saturation = &counters_saturated};
    saturating_counter accepted_pcm_buffers{.saturation = &counters_saturated};
    saturating_counter accepted_pcm_frames{.saturation = &counters_saturated};
    saturating_counter accepted_silent_buffers{.saturation = &counters_saturated};

    std::uint64_t generation{};
    connection_target target{};
    output::backend_handle backend{};
    // Immutable from live publication until the native worker has joined.
    std::optional<output::audio_instance> audio = std::nullopt;
    std::optional<output::body_route_ack> last_admitted_route = std::nullopt;
    std::optional<output::body_playback_identity> last_admitted_playback = std::nullopt;
    std::optional<output::error> last_route_error = std::nullopt;
    std::optional<native_resource_key> candidate_key = std::nullopt;
    std::optional<native_resource_key> reserved_key = std::nullopt;
    bool reservation_held{};
    bool attachment_obligation{};
    bool detach_obligation{};
    bool authority_ready{};
    bool stop_notified{};
    bool telemetry_collected{};
    std::atomic<bool> retry_after_join;
    std::atomic<std::int64_t> identity_valid_until_ticks;
    void* service_context{};
    control_notification* control_wakeup{};
    std::unique_ptr<native_port> port;
    std::unique_ptr<audio_worker> worker;

    std::atomic<setup_phase> setup{setup_phase::empty};
    std::atomic<handoff_phase> handoff{handoff_phase::idle};
    render_request request{};
    render_response response{};
    std::uint64_t next_request_sequence{1U};
};

inline void publish_native_error(audio_slot& slot, const native_error error) noexcept {
    const auto packed = (static_cast<std::uint64_t>(error.operation) << 32U) |
                        static_cast<std::uint64_t>(error.native_code);
    slot.last_native_error_value.store(packed, std::memory_order_relaxed);
    slot.has_native_error.store(true, std::memory_order_release);
}

[[nodiscard]] inline native_error latest_native_error(const audio_slot& slot) noexcept {
    const auto packed = slot.last_native_error_value.load(std::memory_order_relaxed);
    return native_error{.operation = static_cast<native_operation>(packed >> 32U),
        .native_code = static_cast<std::uint32_t>(packed & UINT32_MAX)};
}

struct retry_state final {
    connection_target target{};
    std::uint8_t next_delay_index{};
    std::chrono::steady_clock::time_point not_before;
    bool occupied{};
    std::optional<audio_start_failure> last_start_failure = std::nullopt;
};

struct audio_service::implementation final {
    explicit implementation(audio_dependencies incoming) noexcept
        : runtime(incoming.runtime),
          connections(incoming.connections),
          route_control(incoming.route_control),
          render_control(incoming.render_control),
          native_factory(std::move(incoming.native_factory)),
          worker_launcher(std::move(incoming.worker_launcher)) {}

    gameinput_runtime& runtime;
    gameinput_detail::gameinput_output_connections& connections;
    output::detail::audio_route_control& route_control;
    output::detail::body_render_control& render_control;
    std::unique_ptr<native_port_factory> native_factory;
    std::unique_ptr<audio_worker_launcher> worker_launcher;

    std::array<audio_slot, audio_resident_capacity> slots{};
    std::array<retry_state, gameinput_live_connection_capacity> retries{};
    connection_inventory active_inventory{};
    std::uint64_t next_slot_generation{1U};
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping;
    std::thread control_worker;

    mutable std::mutex publication_mutex;
    control_notification control_wakeup;
    connection_inventory latest_inventory{};
    audio_service_progress published_progress{};
    std::uint64_t retired_admitted_blocks{};
    std::uint64_t retired_source_frames_consumed{};
    std::uint64_t retired_accepted_pcm_buffers{};
    std::uint64_t retired_accepted_pcm_frames{};
    std::uint64_t retired_accepted_silent_buffers{};
    bool retired_counters_saturated{};

    void run_control() noexcept;
    void process_inventory() noexcept;
    void process_slot(audio_slot& slot) noexcept;
    void process_setup(audio_slot& slot) noexcept;
    void reserve_candidate(audio_slot& slot) const noexcept;
    void attach_prepared(audio_slot& slot) noexcept;
    void process_handoff(audio_slot& slot) noexcept;
    void admit_request(audio_slot& slot) noexcept;
    [[nodiscard]] bool obtain_admission(audio_slot& slot,
        const render_request& request,
        std::optional<output::detail::admitted_body_render>& admitted) noexcept;
    void publish_response(audio_slot& slot,
        const render_request& request,
        std::optional<output::detail::admitted_body_render> admitted) noexcept;
    static void cancel_unclaimed_handoff(audio_slot& slot) noexcept;
    static void close_slot(audio_slot& slot) noexcept;
    void settle_attachment(audio_slot& slot) noexcept;
    void collect_worker(audio_slot& slot) noexcept;
    void accumulate_retired_telemetry(audio_slot& slot) noexcept;
    void maybe_start_target(const gameinput_connection& connection) noexcept;
    void publish_progress() noexcept;
    void schedule_retry(connection_target target) noexcept;
    void record_start_failure(audio_start_failure failure) noexcept;
    void clear_retry(connection_target target) noexcept;
    [[nodiscard]] bool desired(connection_target target) const noexcept;
    [[nodiscard]] bool has_resident(connection_target target) const noexcept;
    [[nodiscard]] bool backend_is_current(const audio_slot& slot) const noexcept;
    [[nodiscard]] bool resource_conflicts(const audio_slot& current,
        const native_resource_key& key) const noexcept;
    [[nodiscard]] audio_service_progress snapshot_progress() const noexcept;
    void run_native(audio_slot& slot) noexcept;
    void run_native_stream(audio_slot& slot,
        native_port& port,
        std::uint32_t buffer_size,
        const native_resource_key& resource,
        std::uint64_t generation) noexcept;
    static void native_entry(void* context) noexcept;
};

} // namespace inputnext::audio_detail
