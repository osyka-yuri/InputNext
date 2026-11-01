#include "gameinput_probe/output/session.hpp"

#include "audio/audio_service.hpp"
#include "audio/native_port.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/internal_producer.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/xinput.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

constexpr output::internal_context_options dualsense_output_options{
    .owner = output::internal_owner_key{0x4453'5553'4201ULL},
    .priority = output::internal_priority::diagnostic,
    .ttl = output::ttl_permission::finite_only,
    .foreground = output::foreground_policy::foreground_bound,
};

[[nodiscard]] runtime_id make_runtime_id() {
    GUID guid{};
    const HRESULT result = CoCreateGuid(&guid);
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), "CoCreateGuid");
    }
    runtime_id identity{};
    static_assert(sizeof(guid) == identity.bytes.size());
    std::memcpy(identity.bytes.data(), &guid, sizeof(guid));
    return identity;
}

[[nodiscard]] gameinput_failure resource_failure() noexcept {
    return gameinput_failure{.code = gameinput_error::resource_exhausted, .native_code = 0U};
}

} // namespace

struct output_session::implementation final {
    explicit implementation(std::filesystem::path library)
        : runtime_identity(make_runtime_id()),
          registry(runtime_identity),
          runtime(registry, gameinput_options{.runtime_library = std::move(library)}) {}

    implementation(const implementation&) = delete;
    implementation& operator=(const implementation&) = delete;
    implementation(implementation&&) = delete;
    implementation& operator=(implementation&&) = delete;

    ~implementation() noexcept { static_cast<void>(stop()); }

    [[nodiscard]] gameinput_result<void> start() {
        auto access = runtime_output_access::create(runtime);
        if (!access.has_value()) {
            return std::unexpected(access.error());
        }
        output_access = std::move(*access);

        focus_observer = std::make_unique<focus::observer>(runtime_identity,
            focus::make_current_process_source());
        output_composition = std::make_unique<output::detail::composition>(runtime_identity,
            registry,
            *focus_observer);

        auto created_executor = output::executor::create(output_composition->public_authority());
        if (!created_executor.has_value()) {
            return std::unexpected(resource_failure());
        }
        executor = std::move(*created_executor);

        auto created_bridge = output_composition->make_xinput_rumble();
        if (!created_bridge.has_value()) {
            return std::unexpected(resource_failure());
        }
        presentation = std::make_unique<gameinput_xinput>(runtime,
            *focus_observer,
            std::move(*created_bridge));

        const auto started = runtime.start(maintenance_time::clock::now());
        if (!started.has_value()) {
            return std::unexpected(started.error());
        }
        runtime_started = true;

        auto created_connections = gameinput_output_connections::create(*output_access, *executor);
        if (!created_connections.has_value()) {
            return std::unexpected(created_connections.error());
        }
        connections = std::move(*created_connections);
        return {};
    }

    [[nodiscard]] bool update(const maintenance_time now,
        const bool output_started,
        const std::optional<connection_target> output_target = std::nullopt) {
        if (!runtime_started || runtime_stopped || !presentation) {
            return false;
        }

        runtime.maintain(now);
        latest_inventory = runtime.connections();
        const auto synchronized = presentation->synchronize(latest_inventory);
        if (!synchronized.has_value()) {
            return false;
        }
        presentation->presentation().maintain_output();

        if (output_started) {
            if (!connections) {
                return false;
            }
            connection_inventory selected{};
            const auto* inventory = &latest_inventory;
            if (output_target.has_value()) {
                for (std::size_t index = 0U; index < latest_inventory.size; ++index) {
                    if (latest_inventory.entries[index].target == *output_target) {
                        selected.entries[0U] = latest_inventory.entries[index];
                        selected.size = 1U;
                        break;
                    }
                }
                inventory = &selected;
            }
            if (!connections->synchronize(*inventory).has_value()) {
                return false;
            }
            if (executor) {
                executor->request_drive();
            }
        }
        synchronize_hd_audio();
        return true;
    }

    void synchronize_hd_audio() noexcept {
        if (!hd_audio) {
            return;
        }
        connection_inventory selected{};
        if (hd_audio_target.has_value()) {
            for (std::size_t index = 0U; index < latest_inventory.size; ++index) {
                if (latest_inventory.entries[index].target == *hd_audio_target) {
                    selected.entries[0U] = latest_inventory.entries[index];
                    selected.size = 1U;
                    break;
                }
            }
        }
        hd_audio->synchronize(selected);
    }

    void stop_hd_audio() noexcept {
        if (hd_audio) {
            hd_audio->close_admission();
            hd_audio->request_stop();
            hd_audio->shutdown();
            hd_audio.reset();
        }
        hd_audio_target.reset();
        hd_render_control.reset();
        hd_route_control.reset();
    }

    [[nodiscard]] output_session_stop_report stop() noexcept {
        if (stop_complete) {
            return stop_report;
        }

        stop_hd_audio();
        if (connections) {
            connections->close_admission();
        }
        if (presentation) {
            try {
                presentation->close();
            } catch (...) {
                stop_report.presentation_close_returned = false;
            }
        }
        if (connections) {
            connections->shutdown();
            stop_report.connection_owner_present = true;
            stop_report.connections = connections->progress();
            connections.reset();
        }
        presentation.reset();
        executor.reset();

        while (!runtime_stopped) {
            try {
                stop_report.runtime = runtime.stop(maintenance_time::clock::now());
                runtime_stopped = stop_report.runtime.drained;
                stop_report.runtime_stopped = runtime_stopped;
            } catch (...) {
                stop_report.runtime_stopped = false;
            }
            if (!runtime_stopped) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }

        output_composition.reset();
        if (focus_observer) {
            focus_observer->stop();
            focus_observer.reset();
        }
        output_access.reset();
        stop_complete = true;
        return stop_report;
    }

    const runtime_id runtime_identity;
    device_registry registry;
    gameinput_runtime runtime;
    std::unique_ptr<runtime_output_access> output_access;
    std::unique_ptr<focus::observer> focus_observer;
    std::unique_ptr<output::detail::composition> output_composition;
    std::unique_ptr<output::executor> executor;
    std::unique_ptr<gameinput_xinput> presentation;
    std::unique_ptr<gameinput_output_connections> connections;
    std::optional<output::detail::audio_route_control> hd_route_control;
    std::unique_ptr<output::detail::body_render_control> hd_render_control;
    std::unique_ptr<audio_detail::audio_service> hd_audio;
    std::optional<connection_target> hd_audio_target;
    connection_inventory latest_inventory{};
    output_session_stop_report stop_report{};
    bool runtime_started{};
    bool runtime_stopped{};
    bool stop_complete{};
};

output_session::output_session(std::filesystem::path runtime_library)
    : implementation_(std::make_unique<implementation>(std::move(runtime_library))) {
}

output_session::~output_session() noexcept {
    static_cast<void>(shutdown());
}

gameinput_result<void> output_session::start() {
    return implementation_->start();
}

bool output_session::update(const maintenance_time now, const bool output_started) {
    return implementation_->update(now, output_started);
}

bool output_session::update_targeted_output(const maintenance_time now,
    const connection_target target) {
    return implementation_->update(now, true, target);
}

bool output_session::focus_is_current_foreground() const noexcept {
    if (!implementation_->focus_observer) {
        return false;
    }
    const auto guard = implementation_->focus_observer->lock_presentation();
    return guard.current_foreground_epoch().has_value();
}

focus::snapshot output_session::focus_snapshot() const {
    return implementation_->focus_observer ? implementation_->focus_observer->published()
                                           : focus::snapshot{};
}

connection_inventory output_session::inventory() const {
    return implementation_->latest_inventory;
}

std::array<std::optional<xinput::assignment>, 4U> output_session::assignments() const {
    if (!implementation_->presentation) {
        return {};
    }
    return implementation_->presentation->presentation().positions();
}

std::optional<xinput::rumble_output_status> output_session::output_status(
    const std::uint32_t user_index) const {
    if (!implementation_->presentation) {
        return std::nullopt;
    }
    return implementation_->presentation->presentation().get_rumble_output_status(user_index);
}

gameinput_result<output_description> output_session::describe(
    const connection_target target) const {
    if (!implementation_->output_access) {
        return std::unexpected(
            gameinput_failure{.code = gameinput_error::closed, .native_code = 0U});
    }
    return implementation_->output_access->describe(target);
}

metadata_result<bool> output_session::qualify_dualsense_usb(const connection_target target,
    const std::uint64_t expected_catalogue_generation) const {
    if (!implementation_->output_access) {
        return std::unexpected{metadata_error::closed};
    }
    return implementation_->output_access->qualify_dualsense_usb(target,
        expected_catalogue_generation);
}

metadata_result<metadata_snapshot> output_session::query_metadata(
    const connection_target target) const {
    if (!implementation_->runtime_started || implementation_->runtime_stopped) {
        return std::unexpected{metadata_error::closed};
    }
    return implementation_->runtime.query_metadata(target);
}

output::result<output::detail::internal_producer>
output_session::make_dualsense_diagnostic_producer() & {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->make_producer(dualsense_output_options);
}

bool output_session::start_hd_audio(const connection_target target) {
    auto& state = *implementation_;
    if (state.hd_audio || !state.connections || !state.executor || !state.output_composition ||
        state.stop_complete || !state.runtime_started || !target.valid()) {
        return false;
    }
    const bool target_present = std::ranges::any_of(
        std::span{state.latest_inventory.entries.data(), state.latest_inventory.size},
        [target](const auto& connection) { return connection.target == target; });
    if (!target_present) {
        return false;
    }

    state.hd_route_control.emplace(state.output_composition->audio_routes());
    state.hd_render_control =
        std::make_unique<output::detail::body_render_control>(*state.executor);
    auto created = audio_detail::audio_service::create(audio_detail::audio_dependencies{
        .runtime = state.runtime,
        .connections = *state.connections,
        .route_control = *state.hd_route_control,
        .render_control = *state.hd_render_control,
        .native_factory = audio_detail::make_windows_native_port_factory(),
        .worker_launcher = audio_detail::make_std_audio_worker_launcher(),
    });
    if (!created.has_value()) {
        state.hd_render_control.reset();
        state.hd_route_control.reset();
        return false;
    }
    state.hd_audio = std::move(*created);
    state.hd_audio_target = target;
    state.synchronize_hd_audio();
    return true;
}

bool output_session::retarget_hd_audio(const connection_target target) noexcept {
    auto& state = *implementation_;
    if (!state.hd_audio || state.stop_complete || !target.valid()) {
        return false;
    }
    const bool target_present = std::ranges::any_of(
        std::span{state.latest_inventory.entries.data(), state.latest_inventory.size},
        [target](const auto& connection) { return connection.target == target; });
    if (!target_present) {
        return false;
    }
    state.hd_audio_target = target;
    state.synchronize_hd_audio();
    return true;
}

std::optional<audio_detail::audio_target_progress> output_session::hd_audio_progress(
    const connection_target target) const noexcept {
    return implementation_->hd_audio ? implementation_->hd_audio->progress(target) : std::nullopt;
}

output::result<output::public_context> output_session::open_body_context(
    const connection_target target) {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->public_authority().open(target);
}

output::result<output::prepared_body_pcm>
output_session::prepare_body_pcm(const std::span<const float> samples, const float gain) {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->public_authority().pcm_preparer().prepare(
        output::body_pcm_input{.samples = samples,
            .frame_count = samples.size(),
            .channels = 1U,
            .sample_rate = output::body_pcm_sample_rate,
            .gain = gain});
}

output::result<output::body_playback_receipt> output_session::submit_body_pcm(
    const output::public_context context,
    const output::body_request& request,
    const output::body_playback_update& playback) {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    auto submitted = implementation_->output_composition->public_authority().submit_body(context,
        request,
        playback);
    if (submitted.has_value()) {
        request_drive();
    }
    return submitted;
}

output::result<output::close_receipt> output_session::close_body_context(
    const output::public_context context) {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->public_authority().close(context,
        context.identity.id.value);
}

output::result<void> output_session::acknowledge_body_context_close(
    const output::close_receipt& receipt) {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->public_authority().acknowledge(receipt);
}

void output_session::collect_body_pcm() noexcept {
    if (implementation_->output_composition && !implementation_->stop_complete) {
        implementation_->output_composition->public_authority().collect_body_pcm();
    }
}

std::optional<output::backend_progress> output_session::backend_progress(
    const connection_target target) const noexcept {
    return implementation_->connections
               ? implementation_->connections->query_backend_progress(target)
               : std::nullopt;
}

output::result<output::logical_snapshot> output_session::logical_snapshot(
    const connection_target target) const {
    if (!implementation_->output_composition) {
        return std::unexpected(output::error::not_connected);
    }
    return implementation_->output_composition->public_authority().read_snapshot(target);
}

xinput::service& output_session::presentation() noexcept {
    return implementation_->presentation->presentation();
}

connection_control_report output_session::connection_progress() const noexcept {
    return implementation_->connections ? implementation_->connections->progress()
                                        : connection_control_report{};
}

void output_session::maintain_output() noexcept {
    if (implementation_->presentation) {
        implementation_->presentation->presentation().maintain_output();
    }
}

void output_session::request_drive() noexcept {
    if (implementation_->executor) {
        implementation_->executor->request_drive();
    }
}

output_session_stop_report output_session::shutdown() noexcept {
    return implementation_ ? implementation_->stop() : output_session_stop_report{};
}

} // namespace inputnext::gameinput_detail
