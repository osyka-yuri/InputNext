#pragma once

#include "native_port.hpp"

#include <inputnext/gameinput_runtime.hpp>

#include "gameinput/output/connections/manager.hpp"
#include "output/body/audio_route_control.hpp"
#include "output/render/control.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace inputnext::audio_detail {

// An owned join token for one native worker. A failed join leaves the token resident so the
// service cannot reuse its slot or resource reservation while the worker may still be using it.
class audio_worker {
public:
    audio_worker(const audio_worker&) = delete;
    audio_worker& operator=(const audio_worker&) = delete;
    audio_worker(audio_worker&&) = delete;
    audio_worker& operator=(audio_worker&&) = delete;
    virtual ~audio_worker() noexcept = default;

    [[nodiscard]] virtual bool joinable() const noexcept = 0;
    [[nodiscard]] virtual bool join() noexcept = 0;

protected:
    audio_worker() = default;
};

class audio_worker_launcher {
public:
    using entry_point = void (*)(void*) noexcept;

    audio_worker_launcher(const audio_worker_launcher&) = delete;
    audio_worker_launcher& operator=(const audio_worker_launcher&) = delete;
    audio_worker_launcher(audio_worker_launcher&&) = delete;
    audio_worker_launcher& operator=(audio_worker_launcher&&) = delete;
    virtual ~audio_worker_launcher() noexcept = default;

    // A null result means the worker could not be started. The context remains owned by the
    // caller and must remain stable until the returned worker has joined.
    [[nodiscard]] virtual std::unique_ptr<audio_worker> launch(entry_point entry,
        void* context) noexcept = 0;

protected:
    audio_worker_launcher() = default;
};

[[nodiscard]] std::unique_ptr<audio_worker_launcher> make_std_audio_worker_launcher() noexcept;

struct audio_dependencies final {
    gameinput_runtime& runtime;
    gameinput_detail::gameinput_output_connections& connections;
    output::detail::audio_route_control& route_control;
    output::detail::body_render_control& render_control;
    std::unique_ptr<native_port_factory> native_factory;
    std::unique_ptr<audio_worker_launcher> worker_launcher;
};

enum class audio_service_error : std::uint8_t {
    resource_exhausted,
    worker_start_failed,
};

enum class audio_target_phase : std::uint8_t {
    waiting_for_backend,
    waiting_for_retry,
    opening,
    waiting_for_reservation,
    live,
    revoking,
    draining,
    retained,
};

struct audio_target_progress final {
    connection_target target{};
    std::optional<output::backend_handle> backend = std::nullopt;
    std::optional<output::audio_instance> audio = std::nullopt;
    std::optional<output::body_route_ack> last_admitted_route = std::nullopt;
    std::optional<output::body_playback_identity> last_admitted_playback = std::nullopt;
    std::optional<output::error> last_route_error = std::nullopt;
    std::optional<metadata_error> last_metadata_error = std::nullopt;
    audio_target_phase phase{audio_target_phase::waiting_for_backend};
    bool transport_open{};
    bool native_setup_started{};
    bool native_stream_started{};
    bool ready{};
    bool mmcss_registered{};
    native_operation last_native_operation{native_operation::none};
    std::optional<native_error> last_native_error = std::nullopt;
    std::uint64_t admitted_blocks{};
    std::uint64_t source_frames_consumed{};
    std::uint64_t accepted_pcm_buffers{};
    std::uint64_t accepted_pcm_frames{};
    std::uint64_t accepted_silent_buffers{};
    bool counters_saturated{};
};

struct audio_start_failure final {
    connection_target target{};
    output::backend_handle backend{};
    native_error error{};
};

struct audio_service_progress final {
    bool accepting{true};
    bool stopping{};
    std::size_t desired_targets{};
    std::size_t resident_slots{};
    std::size_t live_slots{};
    std::size_t retained_slots{};
    std::array<audio_target_progress, gameinput_live_connection_capacity> targets{};
    std::size_t target_count{};
    std::uint64_t admitted_blocks{};
    std::uint64_t source_frames_consumed{};
    std::uint64_t accepted_pcm_buffers{};
    std::uint64_t accepted_pcm_frames{};
    std::uint64_t accepted_silent_buffers{};
    bool counters_saturated{};
    std::array<audio_start_failure, gameinput_live_connection_capacity> start_failures{};
    std::size_t start_failure_count{};
};

// One native-session-owned controller. Runtime, connections, route control, and render control
// are borrowed and must outlive shutdown(); the factory and worker launcher are service-owned.
class audio_service final {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<audio_service>, audio_service_error> create(
        audio_dependencies dependencies);
    ~audio_service() noexcept;

    audio_service(const audio_service&) = delete;
    audio_service& operator=(const audio_service&) = delete;
    audio_service(audio_service&&) = delete;
    audio_service& operator=(audio_service&&) = delete;

    void synchronize(const connection_inventory& inventory) noexcept;
    void close_admission() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] audio_service_progress progress() const noexcept;
    [[nodiscard]] std::optional<audio_target_progress> progress(
        connection_target target) const noexcept;
    void shutdown() noexcept;

private:
    struct implementation;
    explicit audio_service(std::unique_ptr<implementation> state) noexcept;

    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::audio_detail
