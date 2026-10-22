#include "state.hpp"

#include "failure.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/rumble/rumble_sink.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include <array>

#include <span>

#include "schedule.hpp"

#include <inputnext/output.hpp>

namespace inputnext::gameinput_detail {
namespace {

using connections_failure::executor_control_failure;
using connections_failure::native_control_failure;
using connections_failure::protocol_failure;
using connections_failure::runtime_control_failure;

[[nodiscard]] bool has_rumble_capability(
    const std::optional<rumble_capabilities>& capabilities) noexcept {
    return capabilities.has_value() &&
           (capabilities->low_frequency || capabilities->high_frequency);
}

[[nodiscard]] bool transient_registration_error(const output::execution_error error) noexcept {
    return error == output::execution_error::resources ||
           error == output::execution_error::capacity_exceeded ||
           error == output::execution_error::busy;
}

} // namespace

void gameinput_output_connections::implementation::scan_descriptions() noexcept {
    std::array<connection_target, desired_capacity> targets{};
    const auto count = snapshot_descriptions(targets);
    for (const auto target : std::span{targets}.first(count)) {
        if (!describe_target(target)) {
            return;
        }
    }
}

std::size_t gameinput_output_connections::implementation::snapshot_descriptions(
    std::array<connection_target, desired_capacity>& targets) noexcept {
    std::scoped_lock lock{gate};
    if (phase != connection_control_phase::accepting || fallback || admissions_frozen) {
        return 0U;
    }

    std::size_t count = 0U;
    const auto now = clock::now();
    for (const auto& entry : desired) {
        if (!connection_schedule::description_due(entry.occupied, now, entry.next_description)) {
            continue;
        }
        targets[count] = entry.target;
        ++count;
    }
    return count;
}

bool gameinput_output_connections::implementation::describe_target(
    const connection_target target) noexcept {
    gameinput_result<output_description> described = std::unexpected{
        gameinput_failure{.code = gameinput_error::native_failure, .native_code = 0U}};
    try {
        described = runtime.describe(target);
    } catch (const std::bad_alloc&) {
        described = std::unexpected{
            gameinput_failure{.code = gameinput_error::resource_exhausted, .native_code = 0U}};
    } catch (...) {
        described = std::unexpected{
            gameinput_failure{.code = gameinput_error::native_failure, .native_code = 0U}};
    }

    const auto completed = clock::now();
    if (!described.has_value()) {
        std::scoped_lock lock{gate};
        auto* const entry = find_desired_locked(target);
        if (entry != nullptr && phase == connection_control_phase::accepting) {
            fail_locked(runtime_control_failure(connection_control_operation::describe,
                target,
                described.error()));
            entry->next_description = completed + std::chrono::milliseconds{64};
        }
        return true;
    }
    if (described->target != target) {
        std::scoped_lock lock{gate};
        fallback_locked(
            protocol_failure(connection_control_operation::describe, target, std::nullopt));
        return false;
    }

    std::scoped_lock lock{gate};
    auto* const entry = find_desired_locked(target);
    if (entry == nullptr || phase != connection_control_phase::accepting) {
        return true;
    }
    const bool material_change = entry->description_known &&
                                 (entry->catalogue_generation != described->catalogue_generation ||
                                     entry->capabilities != described->capabilities);
    entry->description_known = true;
    entry->catalogue_generation = described->catalogue_generation;
    entry->capabilities = described->capabilities;
    entry->next_description = completed + std::chrono::milliseconds{64};
    if (material_change) {
        reset_retry_locked(*entry);
        progress_dirty = true;
    }
    return true;
}

void gameinput_output_connections::implementation::attempt_one_creation() noexcept {
    std::size_t slot = resident_capacity;
    connection_target target{};
    {
        std::scoped_lock lock{gate};
        if (phase != connection_control_phase::accepting || fallback || admissions_frozen) {
            return;
        }
        const auto now = clock::now();
        for (auto& entry : desired) {
            if (!creation_allowed_locked(entry, now)) {
                continue;
            }
            const bool existing_exact = std::ranges::any_of(backends, [&](const auto& record) {
                return record.state != backend_state::free && record.target == entry.target;
            });
            if (existing_exact) {
                continue;
            }
            if (has_same_device_predecessor_locked(entry.target)) {
                entry.resource_waiting = true;
                entry.resource_due = now + std::chrono::milliseconds{64};
                continue;
            }
            slot = find_free_backend_locked();
            if (slot == backends.size()) {
                entry.resource_waiting = true;
                entry.resource_due = now + std::chrono::milliseconds{64};
                continue;
            }
            entry.resource_waiting = false;
            target = entry.target;
            backends[slot] = backend_record{.state = backend_state::creating,
                .target = target,
                .handle = std::nullopt,
                .receipt = std::nullopt,
                .cached_progress = std::nullopt};
            ++candidate_obligations;
            ++creation_handoffs;
            admission_quiescent = false;
            break;
        }
    }
    if (slot < resident_capacity) {
        process_one_creation(slot, target);
    }
}

void gameinput_output_connections::implementation::process_one_creation(const std::size_t slot,
    const connection_target target) noexcept {
    gameinput_result<runtime_output_lease> reserved = std::unexpected{
        gameinput_failure{.code = gameinput_error::native_failure, .native_code = 0U}};
    try {
        reserved = runtime.reserve(target);
    } catch (const std::bad_alloc&) {
        reserved = std::unexpected{
            gameinput_failure{.code = gameinput_error::resource_exhausted, .native_code = 0U}};
    } catch (...) {
        reserved = std::unexpected{
            gameinput_failure{.code = gameinput_error::native_failure, .native_code = 0U}};
    }
    if (!reserved.has_value()) {
        handle_reserve_failure(slot, target, reserved.error());
        return;
    }

    process_reserved_candidate(slot, target, std::move(*reserved));
}

void gameinput_output_connections::implementation::handle_reserve_failure(const std::size_t slot,
    const connection_target target,
    const gameinput_failure failure) noexcept {
    const auto completed = clock::now();
    std::scoped_lock lock{gate};
    auto* const entry = find_desired_locked(target);
    if (entry != nullptr && phase == connection_control_phase::accepting) {
        fail_locked(
            runtime_control_failure(connection_control_operation::reserve, target, failure));
        if (failure.code == gameinput_error::resource_exhausted) {
            arm_retry_locked(*entry, completed);
        } else {
            entry->retry_blocked = true;
            entry->retry_armed = false;
        }
    }
    complete_candidate_locked(slot);
}

bool gameinput_output_connections::implementation::candidate_may_proceed(
    const connection_target target) noexcept {
    std::scoped_lock lock{gate};
    auto const* const entry = find_desired_locked(target);
    return entry != nullptr && phase == connection_control_phase::accepting && !fallback &&
           !admissions_frozen && entry->description_known &&
           has_rumble_capability(entry->capabilities) &&
           !has_same_device_predecessor_locked(target);
}

void gameinput_output_connections::implementation::cancel_candidate(const std::size_t slot,
    const connection_target target) noexcept {
    std::scoped_lock lock{gate};
    if (auto* const entry = find_desired_locked(target);
        entry != nullptr && phase == connection_control_phase::accepting &&
        has_same_device_predecessor_locked(target)) {
        entry->resource_waiting = true;
        entry->resource_due = clock::now() + std::chrono::milliseconds{64};
    }
    complete_candidate_locked(slot);
}

void gameinput_output_connections::implementation::process_reserved_candidate(
    const std::size_t slot,
    const connection_target target,
    runtime_output_lease lease) noexcept {
    if (!candidate_may_proceed(target)) {
        lease = runtime_output_lease{};
        cancel_candidate(slot, target);
        return;
    }

    bool threw = false;
    auto made = make_candidate(std::move(lease), threw);
    if (!made.has_value()) {
        handle_candidate_failure(slot, target, made.error(), threw);
        return;
    }

    auto candidate = std::move(*made);
    if (candidate == nullptr) {
        std::scoped_lock lock{gate};
        fallback_locked(
            protocol_failure(connection_control_operation::create_sink, target, std::nullopt));
        complete_candidate_locked(slot);
        return;
    }
    if (!mark_candidate_registering(slot, target)) {
        candidate.reset();
        cancel_candidate(slot, target);
        return;
    }
    register_candidate(slot, target, std::move(candidate));
}

native_result<std::unique_ptr<output::native_sink>>
gameinput_output_connections::implementation::make_candidate(runtime_output_lease lease,
    bool& threw) noexcept {
    try {
        return make_rumble_sink(std::move(lease));
    } catch (const std::bad_alloc&) {
        return std::unexpected{native_failure{.cause = native_failure_cause::resource_exhausted,
            .native_code = 0U,
            .partial_token = {}}};
    } catch (...) {
        threw = true;
        return std::unexpected{native_failure{.cause = native_failure_cause::native_failure,
            .native_code = 0U,
            .partial_token = {}}};
    }
}

void gameinput_output_connections::implementation::handle_candidate_failure(const std::size_t slot,
    const connection_target target,
    const native_failure failure,
    const bool threw) noexcept {
    const auto completed = clock::now();
    std::scoped_lock lock{gate};
    if (threw) {
        fallback_locked(
            protocol_failure(connection_control_operation::create_sink, target, std::nullopt));
    }
    auto* const entry = find_desired_locked(target);
    if (entry != nullptr && phase == connection_control_phase::accepting) {
        fail_locked(
            native_control_failure(connection_control_operation::create_sink, target, failure));
        if (failure.cause == native_failure_cause::resource_exhausted) {
            arm_retry_locked(*entry, completed);
        } else {
            entry->retry_blocked = true;
            entry->retry_armed = false;
        }
    }
    complete_candidate_locked(slot);
}

bool gameinput_output_connections::implementation::mark_candidate_registering(
    const std::size_t slot,
    const connection_target target) noexcept {
    std::scoped_lock lock{gate};
    auto const* const entry = find_desired_locked(target);
    const bool may_register = entry != nullptr && phase == connection_control_phase::accepting &&
                              !fallback && !admissions_frozen && entry->description_known &&
                              has_rumble_capability(entry->capabilities) &&
                              !has_same_device_predecessor_locked(target);
    if (may_register) {
        backends[slot].state = backend_state::registering;
    }
    return may_register;
}

void gameinput_output_connections::implementation::register_candidate(const std::size_t slot,
    const connection_target target,
    std::unique_ptr<output::native_sink> candidate) noexcept {
    output::execution_result<output::backend_handle> registered =
        std::unexpected{output::execution_error::resources};
    bool threw = false;
    try {
        registered = executor.register_backend(target, candidate, output::rumble_mask);
    } catch (...) {
        threw = true;
    }

    if (threw) {
        candidate.reset();
        retain_unresolved_candidate(slot, target, std::nullopt);
        return;
    }

    if (registered.has_value()) {
        const auto handle = *registered;
        const bool valid_handle = handle.valid() && handle.target == target;
        const bool candidate_transferred = candidate == nullptr;
        if (!valid_handle || !candidate_transferred) {
            candidate.reset();
            retain_unresolved_candidate(slot,
                target,
                handle.valid() ? std::optional{handle} : std::nullopt);
            return;
        }

        publish_registered_candidate(slot, target, handle);
        return;
    }

    const auto failure = registered.error();
    if (candidate == nullptr) {
        retain_unresolved_candidate(slot, target, std::nullopt);
        return;
    }

    handle_registration_failure(slot, target, std::move(candidate), failure);
}

void gameinput_output_connections::implementation::retain_unresolved_candidate(
    const std::size_t slot,
    const connection_target target,
    const std::optional<output::backend_handle> handle) noexcept {
    std::scoped_lock lock{gate};
    auto& record = backends[slot];
    record.state = backend_state::terminal_unresolved;
    record.handle = handle;
    record.target = target;
    ++backend_obligations;
    complete_candidate_locked(slot);
    fallback_locked(
        protocol_failure(connection_control_operation::register_backend, target, handle));
}

void gameinput_output_connections::implementation::publish_registered_candidate(
    const std::size_t slot,
    const connection_target target,
    const output::backend_handle handle) noexcept {
    {
        std::scoped_lock lock{gate};
        auto& record = backends[slot];
        record.state = backend_state::current;
        record.target = target;
        record.handle = handle;
        record.receipt = std::nullopt;
        record.cached_progress = std::nullopt;
        ++backend_obligations;
        --candidate_obligations;
        if (creation_handoffs > 0U) {
            --creation_handoffs;
        }
        admission_quiescent = creation_handoffs == 0U && candidate_obligations == 0U;
        all_retirements_acknowledged = false;
        if (auto* const entry = find_desired_locked(target); entry != nullptr) {
            reset_retry_locked(*entry);
        }
        next_progress_scan = clock::now() + std::chrono::milliseconds{4};
        changed.notify_all();
    }
    executor.request_drive();
}

void gameinput_output_connections::implementation::handle_registration_failure(
    const std::size_t slot,
    const connection_target target,
    std::unique_ptr<output::native_sink> candidate,
    const output::execution_error failure) noexcept {
    candidate.reset();
    const auto completed = clock::now();
    std::scoped_lock lock{gate};
    auto* const entry = find_desired_locked(target);
    if (entry != nullptr && phase == connection_control_phase::accepting) {
        fail_locked(executor_control_failure(connection_control_operation::register_backend,
            target,
            std::nullopt,
            failure));
        if (failure == output::execution_error::identity_exhausted) {
            admissions_frozen = true;
            phase = connection_control_phase::admission_closed;
            admission_dirty = true;
        } else if (failure == output::execution_error::not_connected) {
            entry->retry_blocked = true;
            entry->retry_armed = false;
        } else if (transient_registration_error(failure)) {
            arm_retry_locked(*entry, completed);
        } else {
            fallback_locked(executor_control_failure(connection_control_operation::register_backend,
                target,
                std::nullopt,
                failure));
        }
    } else if (failure == output::execution_error::not_running) {
        fallback_locked(executor_control_failure(connection_control_operation::register_backend,
            target,
            std::nullopt,
            failure));
    }
    complete_candidate_locked(slot);
}

} // namespace inputnext::gameinput_detail
