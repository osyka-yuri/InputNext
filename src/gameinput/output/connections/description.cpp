#include "state.hpp"

#include "backend_selection.hpp"
#include "failure.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "manager.hpp"
#include "schedule.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <mutex>
#include <new>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

using connections_failure::protocol_failure;
using connections_failure::runtime_control_failure;

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

    {
        std::scoped_lock lock{gate};
        auto* const entry = find_desired_locked(target);
        if (entry == nullptr || phase != connection_control_phase::accepting) {
            return true;
        }
        const bool qualification_key_changed =
            !entry->description_known ||
            entry->catalogue_generation != described->catalogue_generation ||
            entry->model != described->model;
        const bool description_changed =
            qualification_key_changed || entry->capabilities != described->capabilities;
        entry->description_known = true;
        entry->catalogue_generation = described->catalogue_generation;
        entry->capabilities = described->capabilities;
        entry->model = described->model;
        entry->next_description = completed + std::chrono::milliseconds{64};
        if (qualification_key_changed) {
            if (entry->qualification != nullptr) {
                entry->qualification->revoke();
            }
            entry->qualification.reset();
            entry->selection.reset();
            entry->next_qualification = clock::time_point::min();
        }
        if (description_changed) {
            if (qualification_key_changed || !needs_usb_qualification(entry->model)) {
                reset_retry_locked(*entry);
            }
            progress_dirty = true;
        }
        if (!needs_usb_qualification(entry->model)) {
            if (entry->qualification != nullptr) {
                entry->qualification->revoke();
                entry->qualification.reset();
            }
            entry->selection = fallback_backend(entry->capabilities);
        }
    }
    return true;
}

} // namespace inputnext::gameinput_detail
