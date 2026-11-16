#pragma once

#include "contribution.hpp"
#include "dinput/native_types.hpp"
#include "output/composition/window_acquisition.hpp"
#include "playback.hpp"
#include "service.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace inputnext::dinput::force_feedback {

// One session-owned state and gate shared by control transactions and the playback worker.
struct service::implementation final {
    using clock = std::chrono::steady_clock;
    static constexpr auto tick_period = std::chrono::milliseconds{4};
    static constexpr DWORD parameter_flags = DIEP_DURATION | DIEP_SAMPLEPERIOD | DIEP_GAIN |
                                             DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL |
                                             DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE |
                                             DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    static constexpr DWORD set_control_flags = DIEP_START | DIEP_NODOWNLOAD | DIEP_NORESTART;
    static constexpr DWORD start_flags = DIES_SOLO | DIES_NODOWNLOAD;

    [[nodiscard]] static bool nonzero(const output::rumble_value& value) noexcept;

    [[nodiscard]] static bool same_token(
        const std::shared_ptr<const output::detail::window_acquisition>& left,
        const std::shared_ptr<const output::detail::window_acquisition>& right) noexcept;

    [[nodiscard]] static HRESULT eligibility_error(const eligibility& acquisition) noexcept;

    [[nodiscard]] static bool eligible(const eligibility& acquisition) noexcept;

    struct effect_entry final {
        const void* device{};
        std::uint64_t id{};
        effect_descriptor descriptor{};
        effect_parameters staged{};
        effect_parameters downloaded{};
        std::shared_ptr<const output::detail::window_acquisition> downloaded_token;
        std::shared_ptr<const output::detail::window_acquisition> active_token;
        playback timeline{};
        clock::time_point started_at;
        DWORD iterations{};
        DWORD pending_parameters{};
        DWORD device_gain{DI_FFNOMINALMAX};
        motors supported{};
        bool actuators_on{true};
        bool has_downloaded{};
        bool playing{};
        bool invalidated{};
    };

    struct acquisition_entry final {
        contribution output;
        motors supported{};
    };

    struct mixed_sample final {
        output::rumble_value value{};
        clock::time_point boundary{clock::time_point::max()};
        bool playing{};
    };

    struct capability_sample final {
        output::context_identity identity{};
        std::shared_ptr<const output::detail::window_acquisition> token;
        motors supported{};
    };

    struct observed_motors final {
        motors supported{};
        std::uint64_t generation{};
    };

    implementation(output::detail::composition& output_composition,
        output::executor& output_executor,
        void* query_context,
        motor_query query) noexcept;

    [[nodiscard]] motors capabilities(connection_target target) const noexcept;

    [[nodiscard]] observed_motors observe_capabilities(const eligibility& acquisition) noexcept;

    [[nodiscard]] std::size_t find_effect(std::uint64_t id) const noexcept;

    [[nodiscard]] std::size_t find_context(
        const std::shared_ptr<const output::detail::window_acquisition>& token) const noexcept;

    void signal_locked() noexcept;

    [[nodiscard]] HRESULT ensure_worker_locked() noexcept;

    [[nodiscard]] HRESULT prepare_start_context_locked(
        const std::shared_ptr<const output::detail::window_acquisition>& token,
        motors supported,
        std::size_t& context_index,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT open_context_locked(
        const std::shared_ptr<const output::detail::window_acquisition>& token,
        std::size_t& index,
        motors supported,
        bool& drive) noexcept;

    void discard_token_locked(
        const std::shared_ptr<const output::detail::window_acquisition>& token) noexcept;

    void reconcile_backend_locked(
        const std::shared_ptr<const output::detail::window_acquisition>& token,
        motors supported,
        bool& drive) noexcept;

    [[nodiscard]] mixed_sample mix_locked(std::size_t index,
        clock::time_point now,
        const effect_entry* replacement = nullptr,
        bool solo = false) noexcept;

    [[nodiscard]] HRESULT publish_context_locked(std::size_t index,
        clock::time_point now,
        clock::time_point& next_boundary,
        bool& drive) noexcept;

    void reconcile_locked(clock::time_point now, bool& drive) noexcept;

    [[nodiscard]] HRESULT
    update_context_locked(std::size_t index, clock::time_point now, bool& drive) noexcept;

    [[nodiscard]] HRESULT
    retire_effect_token_locked(effect_entry& effect, clock::time_point now, bool& drive) noexcept;

    [[nodiscard]] HRESULT stop_device_locked(const void* device,
        std::uint64_t except_id,
        clock::time_point now,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT download_locked(effect_entry& effect,
        const effect_parameters& candidate,
        const eligibility& acquisition,
        motors supported,
        clock::time_point now,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT validate_start_locked(const effect_entry& effect,
        const start_options& options,
        const eligibility& acquisition,
        motors supported,
        effect_parameters& candidate) const noexcept;

    void commit_playback_locked(effect_entry& effect,
        effect_entry&& replacement,
        bool solo,
        bool any_playing,
        contribution& output,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT start_locked(effect_entry& effect,
        const start_options& options,
        const eligibility& acquisition,
        motors supported,
        clock::time_point now,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT replace_playback_locked(effect_entry& effect,
        effect_entry&& replacement,
        const std::shared_ptr<const output::detail::window_acquisition>& token,
        bool solo,
        clock::time_point now,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT set_downloaded_locked(effect_entry& effect,
        DWORD flags,
        const eligibility& acquisition,
        motors supported,
        clock::time_point now,
        bool& drive) noexcept;

    void run_worker() noexcept;

    // Enter and return with gate locked; cached backend queries run while it is released.
    [[nodiscard]] bool refresh_worker_capabilities(std::unique_lock<std::mutex>& lock,
        std::uint64_t observed_generation,
        bool& drive);

    void run_worker_loop();

    [[nodiscard]] HRESULT create(const void* device,
        const effect_descriptor& descriptor,
        const effect_parameters& initial,
        std::uint64_t& id) noexcept;

    [[nodiscard]] HRESULT get(std::uint64_t id,
        DIEFFECT* parameters,
        DWORD flags,
        const compiled_format* format) noexcept;

    [[nodiscard]] HRESULT set(std::uint64_t id,
        const DIEFFECT* parameters,
        DWORD flags,
        const compiled_format* format,
        const eligibility& acquisition,
        observed_motors observed) noexcept;

    [[nodiscard]] HRESULT start(std::uint64_t id,
        const start_options& options,
        const eligibility& acquisition,
        observed_motors observed) noexcept;

    [[nodiscard]] HRESULT
    download(std::uint64_t id, const eligibility& acquisition, observed_motors observed) noexcept;

    [[nodiscard]] HRESULT stop(std::uint64_t id) noexcept;

    [[nodiscard]] HRESULT unload(std::uint64_t id) noexcept;

    [[nodiscard]] HRESULT status(std::uint64_t id, DWORD* result) noexcept;

    [[nodiscard]] HRESULT
    set_actuators_locked(const void* device, bool enabled, bool& drive) noexcept;

    [[nodiscard]] HRESULT reset_device_locked(const void* device, bool& drive) noexcept;

    [[nodiscard]] HRESULT update_contexts_locked(
        const std::array<std::size_t, max_acquisitions>& contexts,
        std::size_t count,
        bool& drive) noexcept;

    [[nodiscard]] HRESULT command(const void* device,
        DWORD command_value,
        const eligibility& acquisition,
        observed_motors observed) noexcept;

    [[nodiscard]] HRESULT state(const void* device,
        DWORD* result,
        const eligibility& acquisition,
        observed_motors observed) noexcept;

    [[nodiscard]] HRESULT set_gain(const void* device, DWORD gain) noexcept;

    void release(std::uint64_t id) noexcept;

    void shutdown() noexcept;

    output::detail::composition& composition;
    output::executor& executor;
    void* capability_context{};
    motor_query query_motors{};
    std::mutex gate;
    std::mutex shutdown_gate;
    std::condition_variable changed;
    std::thread worker;
    std::array<effect_entry, max_effects> effects{};
    std::array<acquisition_entry, max_acquisitions> acquisitions{};
    std::uint64_t next_effect_id{1U};
    std::uint64_t wake_generation{};
    bool admission_closed{};
    bool worker_stop{};
    std::atomic_bool worker_failed;
    bool shutdown_complete{};
};

} // namespace inputnext::dinput::force_feedback
