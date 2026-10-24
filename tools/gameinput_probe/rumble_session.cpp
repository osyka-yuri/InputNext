#include "rumble_session.hpp"

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
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>
#include <inputnext/xinput.hpp>

#include <Windows.h>
#include <objbase.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
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

struct rumble_session::implementation final {
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
        return true;
    }

    [[nodiscard]] rumble_session_stop_report stop() noexcept {
        if (stop_complete) {
            return stop_report;
        }

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
    connection_inventory latest_inventory{};
    rumble_session_stop_report stop_report{};
    bool runtime_started{};
    bool runtime_stopped{};
    bool stop_complete{};
};

rumble_session::rumble_session(std::filesystem::path runtime_library)
    : implementation_(std::make_unique<implementation>(std::move(runtime_library))) {
}

rumble_session::~rumble_session() noexcept {
    static_cast<void>(shutdown());
}

gameinput_result<void> rumble_session::start() {
    return implementation_->start();
}

bool rumble_session::update(const maintenance_time now, const bool output_started) {
    return implementation_->update(now, output_started);
}

bool rumble_session::update_targeted_output(const maintenance_time now,
    const connection_target target) {
    return implementation_->update(now, true, target);
}

bool rumble_session::focus_is_current_foreground() const noexcept {
    if (!implementation_->focus_observer) {
        return false;
    }
    const auto guard = implementation_->focus_observer->lock_presentation();
    return guard.current_foreground_epoch().has_value();
}

focus::snapshot rumble_session::focus_snapshot() const {
    return implementation_->focus_observer ? implementation_->focus_observer->published()
                                           : focus::snapshot{};
}

connection_inventory rumble_session::inventory() const {
    return implementation_->latest_inventory;
}

std::array<std::optional<xinput::assignment>, 4U> rumble_session::assignments() const {
    if (!implementation_->presentation) {
        return {};
    }
    return implementation_->presentation->presentation().positions();
}

std::optional<xinput::rumble_output_status> rumble_session::output_status(
    const std::uint32_t user_index) const {
    if (!implementation_->presentation) {
        return std::nullopt;
    }
    return implementation_->presentation->presentation().get_rumble_output_status(user_index);
}

gameinput_result<output_description> rumble_session::describe(
    const connection_target target) const {
    if (!implementation_->output_access) {
        return std::unexpected(
            gameinput_failure{.code = gameinput_error::closed, .native_code = 0U});
    }
    return implementation_->output_access->describe(target);
}

metadata_result<bool> rumble_session::qualify_dualsense_usb(const connection_target target,
    const std::uint64_t expected_catalogue_generation) const {
    if (!implementation_->output_access) {
        return std::unexpected{metadata_error::closed};
    }
    return implementation_->output_access->qualify_dualsense_usb(target,
        expected_catalogue_generation);
}

output::result<output::detail::internal_producer>
rumble_session::make_dualsense_diagnostic_producer() & {
    if (!implementation_->output_composition || implementation_->stop_complete) {
        return std::unexpected{output::error::not_connected};
    }
    return implementation_->output_composition->make_producer(dualsense_output_options);
}

std::optional<output::backend_progress> rumble_session::backend_progress(
    const connection_target target) const noexcept {
    return implementation_->connections
               ? implementation_->connections->query_backend_progress(target)
               : std::nullopt;
}

output::result<output::logical_snapshot> rumble_session::logical_snapshot(
    const connection_target target) const {
    if (!implementation_->output_composition) {
        return std::unexpected(output::error::not_connected);
    }
    return implementation_->output_composition->public_authority().read_snapshot(target);
}

xinput::service& rumble_session::presentation() noexcept {
    return implementation_->presentation->presentation();
}

connection_control_report rumble_session::connection_progress() const noexcept {
    return implementation_->connections ? implementation_->connections->progress()
                                        : connection_control_report{};
}

void rumble_session::maintain_output() noexcept {
    if (implementation_->presentation) {
        implementation_->presentation->presentation().maintain_output();
    }
}

void rumble_session::request_drive() noexcept {
    if (implementation_->executor) {
        implementation_->executor->request_drive();
    }
}

rumble_session_stop_report rumble_session::shutdown() noexcept {
    return implementation_ ? implementation_->stop() : rumble_session_stop_report{};
}

} // namespace inputnext::gameinput_detail
