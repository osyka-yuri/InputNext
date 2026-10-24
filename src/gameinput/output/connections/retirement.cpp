#include "state.hpp"

#include "backend_selection.hpp"
#include "failure.hpp"
#include "manager.hpp"
#include <inputnext/output_execution.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {
namespace {

using connections_failure::executor_control_failure;
using connections_failure::protocol_failure;

[[nodiscard]] bool terminal_management_error(const output::execution_error error) noexcept {
    return error == output::execution_error::not_running ||
           error == output::execution_error::stale_backend;
}

[[nodiscard]] bool valid_backend_phase(const output::backend_phase phase) noexcept {
    switch (phase) {
    case output::backend_phase::current:
    case output::backend_phase::retiring:
    case output::backend_phase::cleaning:
    case output::backend_phase::drained:
        return true;
    }
    return false;
}

} // namespace

void gameinput_output_connections::implementation::scan_backends() noexcept {
    struct snapshot final {
        std::size_t slot{};
        output::backend_handle handle{};
    };

    std::array<snapshot, resident_capacity> handles{};
    std::size_t count = 0U;
    {
        std::scoped_lock lock{gate};
        if (fallback || phase == connection_control_phase::needs_executor_shutdown ||
            phase == connection_control_phase::stopped) {
            return;
        }
        for (std::size_t index = 0U; index < backends.size(); ++index) {
            const auto& record = backends[index];
            if (record.handle.has_value() && record.state != backend_state::terminal_unresolved) {
                handles[count] = snapshot{.slot = index, .handle = *record.handle};
                ++count;
            }
        }
    }

    for (const auto& item : std::span{handles}.first(count)) {
        if (!process_backend(item.slot, item.handle)) {
            return;
        }
    }
}

bool gameinput_output_connections::implementation::process_backend(const std::size_t slot,
    const output::backend_handle handle) noexcept {
    output::backend_progress progress{};
    auto step = query_backend_progress(handle, progress);
    if (step == backend_scan_step::stop_scan) {
        return false;
    }
    if (step == backend_scan_step::skip_backend) {
        return true;
    }

    std::optional<output::retirement_receipt> receipt;
    bool retire = false;
    step = cache_backend_progress(slot, handle, progress, receipt, retire);
    if (step == backend_scan_step::stop_scan) {
        return false;
    }
    if (step == backend_scan_step::skip_backend) {
        return true;
    }

    step = request_backend_retirement(slot, handle, progress, receipt, retire);
    if (step == backend_scan_step::stop_scan) {
        return false;
    }
    if (step == backend_scan_step::skip_backend) {
        return true;
    }

    return acknowledge_drained_backend(slot, handle, progress, receipt) !=
           backend_scan_step::stop_scan;
}

gameinput_output_connections::implementation::backend_scan_step
gameinput_output_connections::implementation::query_backend_progress(
    const output::backend_handle handle,
    output::backend_progress& progress) noexcept {
    output::execution_result<output::backend_progress> current =
        std::unexpected{output::execution_error::resources};
    try {
        current = executor.progress(handle);
    } catch (...) {
        std::scoped_lock lock{gate};
        fallback_locked(protocol_failure(connection_control_operation::progress_backend,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    if (!current.has_value()) {
        const auto failure =
            executor_control_failure(connection_control_operation::progress_backend,
                handle.target,
                handle,
                current.error());
        std::scoped_lock lock{gate};
        if (terminal_management_error(current.error())) {
            fallback_locked(failure);
            return backend_scan_step::stop_scan;
        }
        fail_locked(failure);
        return backend_scan_step::skip_backend;
    }
    if (current->backend != handle || !valid_backend_phase(current->phase)) {
        std::scoped_lock lock{gate};
        fallback_locked(protocol_failure(connection_control_operation::progress_backend,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    progress = *current;
    return backend_scan_step::progress_ready;
}

gameinput_output_connections::implementation::backend_scan_step
gameinput_output_connections::implementation::cache_backend_progress(const std::size_t slot,
    const output::backend_handle handle,
    const output::backend_progress progress,
    std::optional<output::retirement_receipt>& receipt,
    bool& retire) noexcept {
    std::scoped_lock lock{gate};
    auto const& record = backends[slot];
    if (record.handle != handle || record.state == backend_state::terminal_unresolved) {
        return backend_scan_step::skip_backend;
    }
    const auto expected_channels = represented_channels(record.kind);
    if (expected_channels == 0U || progress.represented_channels != expected_channels ||
        (progress.unsupported_channels & expected_channels) != 0U) {
        fallback_locked(protocol_failure(connection_control_operation::progress_backend,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    if (record.receipt.has_value() && progress.phase == output::backend_phase::current) {
        fallback_locked(protocol_failure(connection_control_operation::progress_backend,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    publish_backend_progress_locked(slot, progress);
    receipt = record.receipt;
    retire = should_retire_locked(record);
    return backend_scan_step::progress_ready;
}

gameinput_output_connections::implementation::backend_scan_step
gameinput_output_connections::implementation::request_backend_retirement(const std::size_t slot,
    const output::backend_handle handle,
    const output::backend_progress progress,
    std::optional<output::retirement_receipt>& receipt,
    const bool should_retire) noexcept {
    const bool needs_receipt = receipt.has_value() || should_retire ||
                               progress.phase == output::backend_phase::retiring ||
                               progress.phase == output::backend_phase::cleaning ||
                               progress.phase == output::backend_phase::drained;
    if (!needs_receipt || receipt.has_value()) {
        return backend_scan_step::progress_ready;
    }

    output::execution_result<output::retirement_receipt> retired =
        std::unexpected{output::execution_error::resources};
    try {
        retired = executor.retire(handle);
    } catch (...) {
        std::scoped_lock lock{gate};
        fallback_locked(
            protocol_failure(connection_control_operation::retire_backend, handle.target, handle));
        return backend_scan_step::stop_scan;
    }
    if (!retired.has_value()) {
        const auto failure = executor_control_failure(connection_control_operation::retire_backend,
            handle.target,
            handle,
            retired.error());
        std::scoped_lock lock{gate};
        if (terminal_management_error(retired.error())) {
            fallback_locked(failure);
            return backend_scan_step::stop_scan;
        }
        if (retired.error() == output::execution_error::resources ||
            retired.error() == output::execution_error::busy ||
            retired.error() == output::execution_error::pending) {
            fail_locked(failure);
            return backend_scan_step::skip_backend;
        }
        fallback_locked(failure);
        return backend_scan_step::stop_scan;
    }
    if (retired->backend != handle) {
        std::scoped_lock lock{gate};
        fallback_locked(
            protocol_failure(connection_control_operation::retire_backend, handle.target, handle));
        return backend_scan_step::stop_scan;
    }

    std::scoped_lock lock{gate};
    auto& record = backends[slot];
    if (record.handle != handle) {
        fallback_locked(
            protocol_failure(connection_control_operation::retire_backend, handle.target, handle));
        return backend_scan_step::stop_scan;
    }
    if (!record.receipt.has_value()) {
        record.receipt = *retired;
        ++pending_retirements;
    } else if (record.receipt != *retired) {
        fallback_locked(
            protocol_failure(connection_control_operation::retire_backend, handle.target, handle));
        return backend_scan_step::stop_scan;
    }
    record.state = backend_state::retiring;
    receipt = *retired;
    return backend_scan_step::progress_ready;
}

gameinput_output_connections::implementation::backend_scan_step
gameinput_output_connections::implementation::acknowledge_drained_backend(const std::size_t slot,
    const output::backend_handle handle,
    const output::backend_progress progress,
    const std::optional<output::retirement_receipt> receipt) noexcept {
    if (progress.phase != output::backend_phase::drained || !receipt.has_value()) {
        return backend_scan_step::skip_backend;
    }
    if (receipt->backend != handle) {
        std::scoped_lock lock{gate};
        fallback_locked(protocol_failure(connection_control_operation::acknowledge_retirement,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }

    output::execution_result<void> acknowledged;
    try {
        acknowledged = executor.acknowledge_retirement(*receipt);
    } catch (...) {
        std::scoped_lock lock{gate};
        fallback_locked(protocol_failure(connection_control_operation::acknowledge_retirement,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    if (!acknowledged.has_value()) {
        std::scoped_lock lock{gate};
        fallback_locked(
            executor_control_failure(connection_control_operation::acknowledge_retirement,
                handle.target,
                handle,
                acknowledged.error()));
        return backend_scan_step::stop_scan;
    }

    std::scoped_lock lock{gate};
    auto& record = backends[slot];
    if (record.handle != handle || record.receipt != receipt ||
        record.state != backend_state::retiring || backend_obligations == 0U ||
        pending_retirements == 0U) {
        fallback_locked(protocol_failure(connection_control_operation::acknowledge_retirement,
            handle.target,
            handle));
        return backend_scan_step::stop_scan;
    }
    record = backend_record{};
    --backend_obligations;
    --pending_retirements;
    all_retirements_acknowledged = backend_obligations == 0U && candidate_obligations == 0U;
    for (auto& entry : desired) {
        if (entry.occupied && entry.resource_waiting) {
            entry.resource_waiting = false;
        }
    }
    progress_dirty = true;
    changed.notify_one();
    return backend_scan_step::skip_backend;
}

} // namespace inputnext::gameinput_detail
