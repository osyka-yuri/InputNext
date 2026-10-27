#pragma once

#include "audio/audio_service.hpp"
#include "core/core.hpp"
#include "effects/effects.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "native_session.hpp"
#include "native_session_dependencies.hpp"
#include "output/body/audio_route_control.hpp"
#include "output/composition/composition.hpp"
#include "output/render/control.hpp"
#include "profiles/application.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/output_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace inputnext::runtime_detail {

struct native_session::implementation final {
    explicit implementation(runtime_id identity);
    ~implementation() noexcept;

    [[nodiscard]] static std::unique_ptr<native_session> prepare(
        native_session_dependencies dependencies);

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    void launch(worker_launcher launcher, void* launcher_context);
    void maintenance_loop() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] bool close_admission_and_presentation() noexcept;
    void drain_owned_domains() noexcept;
    void shutdown() noexcept;
    void record_stop_report(const gameinput_stop_report& report) noexcept;

    [[nodiscard]] bool should_stop() const noexcept;
    [[nodiscard]] std::optional<gameinput_failure> run_maintenance_cycle();
    void wait_for_next_cycle();
    void terminate_after_failure(gameinput_failure failure) noexcept;

    const runtime_id identity;
    device_registry registry;
    std::unique_ptr<gameinput_runtime> runtime;
    std::unique_ptr<gameinput_detail::runtime_output_access> runtime_access;
    std::unique_ptr<focus::observer> focus_observer;
    std::unique_ptr<output::detail::composition> output_composition;
    std::unique_ptr<output::executor> output_executor;
    std::unique_ptr<gameinput_detail::gameinput_output_connections> connections;
    std::unique_ptr<output::detail::audio_route_control> audio_routes;
    std::unique_ptr<output::detail::body_render_control> body_render;
    std::unique_ptr<audio_detail::audio_service> audio;
    std::unique_ptr<gameinput_xinput> presentation_service;
    std::unique_ptr<profiles::application> profile_application;
    std::unique_ptr<core::service> core_service;
    std::unique_ptr<effects::service> effects_service;
    std::thread worker;

    mutable std::mutex control_gate;
    mutable std::mutex lifecycle_gate;
    std::condition_variable lifecycle_changed;
    bool start_attempted{};
    bool start_completed{};
    bool startup_succeeded{};
    bool activated{};
    bool stop_requested{};
    std::optional<gameinput_failure> startup_failure;
    std::optional<gameinput_failure> maintenance_failure;

    std::mutex shutdown_gate;
    std::mutex drain_gate;
    bool domains_drained{};
    bool shutdown_finished{};

    std::atomic<native_session_drain_phase> drain_phase{native_session_drain_phase::not_started};
    std::atomic<bool> drain_started;
    std::atomic<std::chrono::steady_clock::duration::rep> drain_started_ticks;
    std::atomic<std::uint64_t> drain_stop_attempts;
    std::atomic<std::uint64_t> drain_exception_count;
    std::atomic<std::size_t> resident_audio_slots{0U};
    std::atomic<std::size_t> retained_audio_slots{0U};
    std::atomic<bool> has_last_stop_report;
    std::atomic<bool> last_stop_drained;
    std::atomic<std::size_t> last_pending_callback_owners;
    std::atomic<std::size_t> last_active_metadata_queries;
    std::atomic<std::size_t> last_active_history_calls;
    std::atomic<std::size_t> last_pending_history_owners;
    std::atomic<std::size_t> last_pending_output_leases;
    std::atomic<std::size_t> last_pending_native_cleanup_owners;
    std::atomic<bool> last_native_api_cleanup_complete;

    std::uint64_t qpc_frequency{};
};

} // namespace inputnext::runtime_detail
