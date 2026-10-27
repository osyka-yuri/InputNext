#include "manager.hpp"

#include "backend_selection.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "state.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] gameinput_failure runtime_failure(const gameinput_error code) noexcept {
    return gameinput_failure{.code = code, .native_code = 0U};
}

[[nodiscard]] bool has_valid_target(const gameinput_connection& connection) noexcept {
    return connection.target.valid();
}

[[nodiscard]] bool same_device(const connection_target left,
    const connection_target right) noexcept {
    return left.runtime == right.runtime && left.device == right.device;
}

[[nodiscard]] bool conflicting_device(const std::span<const connection_target> targets,
    const connection_target candidate) noexcept {
    return std::ranges::any_of(targets, [&](const connection_target target) {
        return same_device(target, candidate) && target != candidate;
    });
}

} // namespace

gameinput_output_connections::implementation::implementation(runtime_output_access& runtime_access,
    output::executor& output_executor) noexcept
    : runtime(runtime_access),
      executor(output_executor) {
}

gameinput_output_connections::implementation::~implementation() noexcept = default;

gameinput_output_connections::gameinput_output_connections(
    std::unique_ptr<implementation> state) noexcept
    : implementation_(std::move(state)) {
}

gameinput_result<std::unique_ptr<gameinput_output_connections>>
gameinput_output_connections::create(runtime_output_access& runtime, output::executor& executor) {
    std::unique_ptr<gameinput_output_connections> owner;
    try {
        auto state = std::make_unique<implementation>(runtime, executor);
        owner.reset(new gameinput_output_connections(std::move(state)));
        auto* const state_pointer = owner->implementation_.get();
        state_pointer->worker = std::thread([state_pointer] { state_pointer->worker_loop(); });
    } catch (const std::bad_alloc&) {
        if (owner != nullptr) {
            owner->implementation_.reset();
        }
        owner.reset();
        return std::unexpected{runtime_failure(gameinput_error::resource_exhausted)};
    } catch (const std::system_error&) {
        if (owner != nullptr) {
            owner->implementation_.reset();
        }
        owner.reset();
        return std::unexpected{runtime_failure(gameinput_error::resource_exhausted)};
    }
    return std::move(owner);
}

gameinput_output_connections::~gameinput_output_connections() noexcept {
    if (implementation_ != nullptr) {
        shutdown();
    }
}

gameinput_result<void> gameinput_output_connections::synchronize(
    const connection_inventory& inventory) {
    if (inventory.size > inventory.entries.size()) {
        return std::unexpected{runtime_failure(gameinput_error::invalid_argument)};
    }

    std::array<connection_target, implementation::desired_capacity> targets{};
    std::size_t target_count = 0U;
    for (const auto& connection : std::span{inventory.entries}.first(inventory.size)) {
        if (!has_valid_target(connection)) {
            continue;
        }
        const auto duplicate =
            std::ranges::find(std::span{targets}.first(target_count), connection.target);
        if (duplicate != std::span{targets}.first(target_count).end()) {
            continue;
        }
        if (conflicting_device(std::span{targets}.first(target_count), connection.target)) {
            return std::unexpected{runtime_failure(gameinput_error::invalid_argument)};
        }
        targets[target_count] = connection.target;
        ++target_count;
    }

    {
        std::scoped_lock lock{implementation_->gate};
        if (implementation_->phase != connection_control_phase::accepting) {
            return std::unexpected{runtime_failure(gameinput_error::closed)};
        }
        if (implementation_->admissions_frozen) {
            return std::unexpected{runtime_failure(gameinput_error::resource_exhausted)};
        }

        const auto current_targets = std::span{targets}.first(target_count);
        std::array<implementation::desired_record, implementation::desired_capacity> next{};
        for (std::size_t index = 0U; index < target_count; ++index) {
            const auto existing =
                std::ranges::find_if(implementation_->desired, [&](const auto& record) {
                    return record.occupied && record.target == targets[index];
                });
            if (existing != implementation_->desired.end()) {
                next[index] = *existing;
                continue;
            }
            next[index].occupied = true;
            next[index].target = targets[index];
            next[index].next_description = implementation::clock::time_point::min();
            next[index].next_qualification = implementation::clock::time_point::min();
        }
        for (const auto& entry : implementation_->desired) {
            if (!entry.occupied ||
                std::ranges::find(current_targets, entry.target) != current_targets.end()) {
                continue;
            }
            if (entry.qualification != nullptr) {
                entry.qualification->revoke();
            }
        }
        implementation_->desired = std::move(next);
        implementation_->inventory_dirty = true;
        implementation_->progress_dirty = true;
        implementation_->changed.notify_one();
    }
    return {};
}

void gameinput_output_connections::close_admission() noexcept {
    std::scoped_lock lock{implementation_->gate};
    if (implementation_->phase == connection_control_phase::accepting) {
        implementation_->phase = connection_control_phase::admission_closed;
        implementation_->admission_dirty = true;
        implementation_->progress_dirty = true;
        implementation_->runtime.close_admission();
        implementation_->runtime_admission_closed = true;
        implementation_->changed.notify_one();
    }
}

void gameinput_output_connections::request_stop() noexcept {
    close_admission();
    {
        std::scoped_lock lock{implementation_->gate};
        if (implementation_->phase == connection_control_phase::stopped ||
            implementation_->phase == connection_control_phase::needs_executor_shutdown ||
            implementation_->control_finished) {
            return;
        }
        for (auto const& entry : implementation_->desired) {
            if (entry.qualification != nullptr) {
                entry.qualification->revoke();
            }
        }
        for (auto const& backend : implementation_->backends) {
            if (backend.qualification != nullptr) {
                backend.qualification->revoke();
            }
        }
        implementation_->phase = connection_control_phase::stopping;
        implementation_->desired = {};
        implementation_->stop_dirty = true;
        implementation_->progress_dirty = true;
        implementation_->changed.notify_one();
    }
}

connection_control_report gameinput_output_connections::progress() const noexcept {
    std::scoped_lock lock{implementation_->gate};
    return implementation_->report_locked();
}

std::optional<output::backend_progress> gameinput_output_connections::backend_progress(
    const connection_target target) const noexcept {
    std::scoped_lock lock{implementation_->gate};
    const implementation::backend_record* retiring = nullptr;
    for (const auto& record : implementation_->backends) {
        if (record.state == implementation::backend_state::free || record.target != target ||
            !record.cached_progress.has_value()) {
            continue;
        }
        if (record.state == implementation::backend_state::current) {
            return record.cached_progress;
        }
        if (retiring == nullptr) {
            retiring = &record;
        }
    }
    return retiring == nullptr ? std::nullopt : retiring->cached_progress;
}

std::optional<output::backend_progress> gameinput_output_connections::query_backend_progress(
    const connection_target target) const noexcept {
    const auto managed = backend_progress(target);
    if (!managed.has_value()) {
        return std::nullopt;
    }
    try {
        const auto current = implementation_->executor.progress(managed->backend);
        return current.has_value() ? std::optional{*current} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

output::execution_result<std::optional<managed_backend_observation>>
gameinput_output_connections::observe_backend(const connection_target target) const {
    output::backend_handle handle{};
    output_backend_kind kind = output_backend_kind::none;
    {
        std::scoped_lock lock{implementation_->gate};
        const auto current =
            std::ranges::find_if(implementation_->backends, [target](const auto& record) {
                return record.state == implementation::backend_state::current &&
                       record.target == target && record.handle.has_value();
            });
        if (current == implementation_->backends.end()) {
            return std::optional<managed_backend_observation>{};
        }
        if (!current->handle.has_value()) {
            return std::optional<managed_backend_observation>{};
        }
        handle = current->handle.value();
        if (!handle.valid() || handle.target != target ||
            current->kind == output_backend_kind::none) {
            return std::unexpected{output::execution_error::stale_backend};
        }
        kind = current->kind;
    }

    const auto observed = implementation_->executor.observe(handle);
    {
        std::scoped_lock lock{implementation_->gate};
        const auto current =
            std::ranges::find_if(implementation_->backends, [target](const auto& record) {
                return record.state == implementation::backend_state::current &&
                       record.target == target;
            });
        if (current == implementation_->backends.end() || !current->handle.has_value() ||
            current->handle.value() != handle || current->kind != kind) {
            return std::unexpected{output::execution_error::busy};
        }
    }
    if (!observed.has_value()) {
        if (observed.error() == output::execution_error::stale_backend) {
            return std::unexpected{output::execution_error::busy};
        }
        return std::unexpected{observed.error()};
    }
    return std::optional<managed_backend_observation>{managed_backend_observation{
        .kind = kind,
        .execution = *observed,
    }};
}

void gameinput_output_connections::shutdown() noexcept {
    std::scoped_lock shutdown_lock{implementation_->shutdown_gate};
    {
        std::scoped_lock lock{implementation_->gate};
        if (implementation_->executor_shutdown_returned) {
            return;
        }
    }

    request_stop();
    if (implementation_->worker.joinable()) {
        implementation_->worker.join();
    }
    {
        std::scoped_lock lock{implementation_->gate};
        implementation_->control_joined = true;
        implementation_->phase = connection_control_phase::needs_executor_shutdown;
    }

    implementation_->executor.shutdown();

    {
        std::scoped_lock lock{implementation_->gate};
        implementation_->executor_shutdown_returned = true;
        if (implementation_->fallback) {
            for (auto& record : implementation_->backends) {
                record = implementation::backend_record{};
            }
            implementation_->backend_obligations = 0U;
            implementation_->pending_retirements = 0U;
            implementation_->all_retirements_acknowledged = false;
        } else {
            implementation_->all_retirements_acknowledged =
                implementation_->backend_obligations == 0U &&
                implementation_->candidate_obligations == 0U;
        }
        implementation_->phase = connection_control_phase::stopped;
        implementation_->changed.notify_all();
    }
}

} // namespace inputnext::gameinput_detail
