#include "state.hpp"

#include "backend_selection.hpp"
#include "failure.hpp"
#include "manager.hpp"
#include <inputnext/device_metadata.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

using connections_failure::metadata_control_failure;

[[nodiscard]] bool qualification_phase(const connection_control_phase phase) noexcept {
    return phase == connection_control_phase::accepting ||
           phase == connection_control_phase::admission_closed;
}

} // namespace

std::size_t gameinput_output_connections::implementation::snapshot_qualifications(
    std::array<qualification_request, desired_capacity>& requests) noexcept {
    std::scoped_lock lock{gate};
    if (fallback || !qualification_phase(phase)) {
        return 0U;
    }

    const auto now = clock::now();
    std::size_t count = 0U;
    for (const auto& entry : desired) {
        if (!entry.occupied || !entry.description_known || !needs_usb_qualification(entry.model) ||
            now < entry.next_qualification) {
            continue;
        }

        if (phase == connection_control_phase::admission_closed &&
            !has_usb_route_locked(entry.target)) {
            continue;
        }

        requests[count] = qualification_request{.target = entry.target,
            .catalogue_generation = entry.catalogue_generation,
            .qualification = entry.qualification};
        ++count;
    }
    return count;
}

void gameinput_output_connections::implementation::scan_qualifications() noexcept {
    std::array<qualification_request, desired_capacity> requests{};
    const auto count = snapshot_qualifications(requests);
    for (const auto& request : std::span{requests}.first(count)) {
        qualify_selection(request);
    }
}

gameinput_output_connections::implementation::desired_record*
gameinput_output_connections::implementation::current_qualification_locked(
    const qualification_request& request) noexcept {
    auto* const entry = find_desired_locked(request.target);
    if (entry == nullptr || !qualification_phase(phase) || fallback || !entry->description_known ||
        entry->catalogue_generation != request.catalogue_generation ||
        !needs_usb_qualification(entry->model) || entry->qualification != request.qualification) {
        return nullptr;
    }
    return entry;
}

void gameinput_output_connections::implementation::reject_qualification(
    const qualification_request& request,
    const std::optional<metadata_error> failure,
    const bool known_ineligible) noexcept {
    std::scoped_lock lock{gate};
    auto* const entry = current_qualification_locked(request);
    if (entry == nullptr) {
        return;
    }
    if (entry->qualification != nullptr) {
        entry->qualification->revoke();
    }
    entry->qualification.reset();
    entry->selection = known_ineligible ? std::optional{output_backend_kind::none} : std::nullopt;
    entry->next_qualification = clock::now() + usb_output_qualification::refresh_interval;
    if (failure.has_value()) {
        fail_locked(metadata_control_failure(request.target, *failure));
    }
    progress_dirty = true;
    changed.notify_one();
}

void gameinput_output_connections::implementation::qualify_selection(
    const qualification_request& request) noexcept {
    const auto query_started = clock::now();
    metadata_result<bool> qualified = std::unexpected{metadata_error::resource_exhausted};
    bool threw{};
    try {
        qualified = runtime.qualify_dualsense_usb(request.target, request.catalogue_generation);
    } catch (const std::bad_alloc&) {
        qualified = std::unexpected{metadata_error::resource_exhausted};
    } catch (...) {
        threw = true;
    }
    const auto completed = clock::now();
    const bool timely = completed < query_started + usb_output_qualification::validity;

    if (threw) {
        std::scoped_lock lock{gate};
        if (auto* const entry = current_qualification_locked(request); entry != nullptr) {
            entry->selection.reset();
            fallback_locked(
                connections_failure::protocol_failure(connection_control_operation::qualify_backend,
                    request.target,
                    std::nullopt));
        }
        return;
    }
    if (!qualified.has_value()) {
        reject_qualification(request, qualified.error(), false);
        return;
    }
    if (!*qualified) {
        reject_qualification(request, std::nullopt, true);
        return;
    }
    if (!timely) {
        reject_qualification(request, metadata_error::stale, false);
        return;
    }

    {
        std::scoped_lock lock{gate};
        auto* const entry = current_qualification_locked(request);
        if (entry == nullptr) {
            return;
        }
        entry->next_qualification = completed + usb_output_qualification::refresh_interval;
        const auto installed = clock::now();
        if (request.qualification != nullptr &&
            request.qualification->renew({.started = query_started, .completed = installed})) {
            entry->selection = output_backend_kind::dualsense_usb;
            progress_dirty = true;
            changed.notify_one();
            return;
        }
        if (request.qualification != nullptr) {
            request.qualification->revoke();
        }
        entry->selection.reset();
        progress_dirty = true;
    }

    std::shared_ptr<usb_output_qualification> fresh;
    try {
        fresh = std::make_shared<usb_output_qualification>(request.target,
            request.catalogue_generation,
            query_started);
    } catch (const std::bad_alloc&) {
        std::scoped_lock lock{gate};
        if (auto* const entry = current_qualification_locked(request); entry != nullptr) {
            entry->next_qualification = completed + usb_output_qualification::refresh_interval;
            fail_locked(
                metadata_control_failure(request.target, metadata_error::resource_exhausted));
            changed.notify_one();
        }
        return;
    }

    std::scoped_lock lock{gate};
    auto* const entry = current_qualification_locked(request);
    if (entry == nullptr) {
        fresh->revoke();
        return;
    }
    const auto installed = clock::now();
    if (installed >= query_started + usb_output_qualification::validity) {
        fresh->revoke();
        entry->next_qualification = installed + usb_output_qualification::refresh_interval;
        fail_locked(metadata_control_failure(request.target, metadata_error::stale));
        changed.notify_one();
        return;
    }
    entry->qualification = std::move(fresh);
    entry->selection = output_backend_kind::dualsense_usb;
    entry->next_qualification = completed + usb_output_qualification::refresh_interval;
    reset_retry_locked(*entry);
    progress_dirty = true;
    changed.notify_one();
}

} // namespace inputnext::gameinput_detail
