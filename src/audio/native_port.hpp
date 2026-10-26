#pragma once

#include "native_types.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace inputnext::audio_detail {

// wake_worker() and request_stop() signal the owning native worker from another thread.
// All other operations, including finish_worker_sta(), belong to that worker.
class native_port {
public:
    native_port(const native_port&) = delete;
    native_port& operator=(const native_port&) = delete;
    native_port(native_port&&) = delete;
    native_port& operator=(native_port&&) = delete;
    virtual ~native_port() noexcept = default;

    [[nodiscard]] virtual native_notification_result wake_worker() noexcept = 0;
    [[nodiscard]] virtual native_notification_result request_stop() noexcept = 0;

    [[nodiscard]] virtual native_result<void> begin_worker_sta() noexcept = 0;
    [[nodiscard]] virtual native_result<native_endpoint_snapshot> open_exact_endpoint(
        const fixed_haptic_endpoint_id& id) noexcept = 0;
    // Refreshes the currently held IMMDevice in place; it never reopens or replaces the endpoint.
    // A changed actual endpoint ID is reported as an error.
    [[nodiscard]] virtual native_result<native_endpoint_snapshot> revalidate_exact_endpoint(
        const fixed_haptic_endpoint_id& id) noexcept = 0;
    [[nodiscard]] virtual native_result<void> activate_audio_client() noexcept = 0;
    [[nodiscard]] virtual native_result<native_audio_format> get_mix_format() noexcept = 0;
    [[nodiscard]] virtual native_result<native_format_support_result> is_format_supported(
        const native_audio_format& format) noexcept = 0;

    // Performs shared event-mode Initialize and immediately associates this port's buffer event
    // before returning, even when cancellation races with setup.
    [[nodiscard]] virtual native_result<void> initialize_event_driven_stream(
        const native_audio_format& format) noexcept = 0;
    [[nodiscard]] virtual native_result<void> acquire_render_service() noexcept = 0;
    [[nodiscard]] virtual native_result<std::uint32_t> get_buffer_size() noexcept = 0;
    [[nodiscard]] virtual native_result<void> start_stream() noexcept = 0;
    [[nodiscard]] virtual native_result<void> stop_stream() noexcept = 0;
    [[nodiscard]] virtual native_result<std::uint32_t> get_current_padding() noexcept = 0;

    // The returned four-channel float view aliases WASAPI memory and remains valid only until the
    // paired release. Callers must qualify that format first. A successful nonzero acquisition is
    // paired with exactly one same-worker release.
    [[nodiscard]] virtual native_result<std::span<float>> acquire_render_buffer(
        std::uint32_t frame_count) noexcept = 0;
    [[nodiscard]] virtual native_result<void> release_render_buffer(
        native_buffer_disposition disposition) noexcept = 0;

    [[nodiscard]] virtual native_wait_result wait_for_activity(
        std::chrono::milliseconds timeout) noexcept = 0;
    [[nodiscard]] virtual native_mmcss_registration register_mmcss_audio() noexcept = 0;

    // Stops if Start was attempted, releases render/audio/device/enumerator COM objects,
    // closes the associated buffer event, reverts MMCSS, then calls CoUninitialize. Wake/stop
    // handles remain owned until this object is destroyed after the worker has joined.
    [[nodiscard]] virtual native_result<native_cleanup_report> finish_worker_sta() noexcept = 0;

protected:
    native_port() = default;
};

class native_port_factory {
public:
    native_port_factory(const native_port_factory&) = delete;
    native_port_factory& operator=(const native_port_factory&) = delete;
    native_port_factory(native_port_factory&&) = delete;
    native_port_factory& operator=(native_port_factory&&) = delete;
    virtual ~native_port_factory() noexcept = default;

    // Creates one independent set of wake, stop, and buffer events. Partial allocation failure
    // closes any handles already created. The returned port must outlive its worker and join.
    [[nodiscard]] virtual native_result<std::unique_ptr<native_port>> create() noexcept = 0;

protected:
    native_port_factory() = default;
};

[[nodiscard]] std::unique_ptr<native_port_factory> make_windows_native_port_factory() noexcept;

} // namespace inputnext::audio_detail
