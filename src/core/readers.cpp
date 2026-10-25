#include "core_state.hpp"

#include "core.hpp"
#include "history_status.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>

namespace inputnext::core {

std::optional<std::size_t> service::implementation::find_reader_locked(
    const std::uint64_t id) const noexcept {
    if (id == 0U) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < readers.size(); ++index) {
        if (readers[index].occupied && readers[index].native.value == id) {
            return index;
        }
    }
    return std::nullopt;
}

void service::implementation::release_reader_if_drained_locked(reader_entry& entry) noexcept {
    if (entry.occupied && entry.closing && entry.close_completed && !entry.open_inflight &&
        !entry.read_inflight && !entry.close_inflight) {
        entry = reader_entry{};
    }
}

status service::implementation::close_native_reader(const std::size_t index) {
    history_reader_handle native{};
    {
        std::scoped_lock lock{reader_gate};
        if (index >= readers.size() || !readers[index].occupied || !readers[index].closing) {
            return status::stale_reader;
        }
        auto& entry = readers[index];
        if (entry.close_completed) {
            return status::stale_reader;
        }
        if (entry.close_inflight || entry.open_inflight) {
            return status::busy;
        }
        entry.close_inflight = true;
        native = entry.native;
    }

    history_result<void> closed =
        std::unexpected(history_failure{.code = history_error::provider_failure});
    try {
        closed = runtime.close_history(native);
    } catch (const std::bad_alloc&) {
        closed = std::unexpected(history_failure{.code = history_error::capacity_exceeded});
    } catch (...) {
        closed = std::unexpected(history_failure{.code = history_error::provider_failure});
    }

    std::scoped_lock lock{reader_gate};
    if (index >= readers.size() || !readers[index].occupied || readers[index].native != native) {
        return status::stale_reader;
    }
    auto& entry = readers[index];
    entry.close_inflight = false;
    if (closed.has_value()) {
        entry.close_completed = true;
        entry.pending.reset();
        release_reader_if_drained_locked(entry);
        return status::ok;
    }

    if (detail::cleanup_completed(closed.error().code)) {
        entry.close_completed = true;
        entry.pending.reset();
        release_reader_if_drained_locked(entry);
        return status::ok;
    }
    return detail::map_history_failure(closed.error().code);
}

void service::implementation::retry_deferred_closes() {
    for (std::size_t index = 0U; index < readers.size(); ++index) {
        bool retry{};
        {
            std::scoped_lock lock{reader_gate};
            const auto& entry = readers[index];
            retry = entry.occupied && entry.closing && !entry.close_completed &&
                    !entry.close_inflight && !entry.open_inflight;
        }
        if (retry) {
            static_cast<void>(close_native_reader(index));
        }
    }
}

result<service::implementation::open_operation>
service::implementation::prepare_open(const connection_target target, const reader_focus mode) {
    if (!target.valid()) {
        return std::unexpected(status::invalid_argument);
    }
    if (target.runtime != identity) {
        return std::unexpected(status::stale_target);
    }
    if (mode != reader_focus::foreground_process && mode != reader_focus::background) {
        return std::unexpected(status::invalid_argument);
    }
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    retry_deferred_closes();
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    std::optional<focus::foreground_epoch> epoch;
    if (mode == reader_focus::foreground_process) {
        const auto guard = focus_observer.lock_presentation();
        epoch = guard.current_foreground_epoch();
        if (!epoch.has_value()) {
            return std::unexpected(status::not_admitted);
        }
    }

    auto target_pin = registry.capture(target);
    if (!target_pin.has_value()) {
        return std::unexpected(status::stale_target);
    }
    target_pin->release();

    std::size_t slot{};
    {
        std::scoped_lock lock{reader_gate};
        if (admission_closed.load(std::memory_order_acquire)) {
            return std::unexpected(status::unavailable);
        }
        const auto available = std::ranges::find_if(readers,
            [](const reader_entry& entry) { return !entry.occupied; });
        if (available == readers.end()) {
            return std::unexpected(status::capacity_exceeded);
        }
        slot = static_cast<std::size_t>(std::distance(readers.begin(), available));
        *available = reader_entry{.focus = mode, .occupied = true, .open_inflight = true};
    }

    return open_operation{.target = target, .focus = mode, .epoch = epoch, .slot = slot};
}

result<void> service::implementation::open_native(open_operation& operation) {
    history_result<history_reader_handle> opened =
        std::unexpected(history_failure{.code = history_error::provider_failure});
    try {
        opened = runtime.open_history(operation.target, history_time::clock::now());
    } catch (const std::bad_alloc&) {
        opened = std::unexpected(history_failure{.code = history_error::capacity_exceeded});
    } catch (...) {
        opened = std::unexpected(history_failure{.code = history_error::provider_failure});
    }

    if (!opened.has_value()) {
        std::scoped_lock lock{reader_gate};
        readers[operation.slot] = reader_entry{};
        const auto failure_status = opened.error().code == history_error::disconnected
                                        ? status::stale_target
                                        : detail::map_history_failure(opened.error().code);
        return std::unexpected(failure_status);
    }

    const auto native = *opened;
    if (native.target != operation.target || native.value == 0U) {
        if (!native.target.valid() || native.value == 0U) {
            std::scoped_lock lock{reader_gate};
            // The underlying store only publishes valid handles; without one it has no addressable
            // owner to close and the malformed result cannot be exposed to a caller.
            readers[operation.slot] = reader_entry{};
            return std::unexpected(status::io_failed);
        }
        {
            std::scoped_lock lock{reader_gate};
            auto& entry = readers[operation.slot];
            entry.native = native;
            entry.open_inflight = false;
            entry.closing = true;
        }
        static_cast<void>(close_native_reader(operation.slot));
        return std::unexpected(status::stale_target);
    }

    {
        std::scoped_lock lock{reader_gate};
        readers[operation.slot].native = native;
    }
    operation.native = native;
    return {};
}

status service::implementation::commit_open(open_operation& operation,
    const focus::observer::presentation_guard* foreground_guard) {
    std::scoped_lock lock{reader_gate};
    auto& entry = readers[operation.slot];
    entry.open_inflight = false;
    entry.closing = true;
    if (admission_closed.load(std::memory_order_acquire)) {
        return status::unavailable;
    }

    try {
        // This final capture orders a successful open before later target retirement.
        const auto current = registry.capture(operation.target);
        if (admission_closed.load(std::memory_order_acquire)) {
            return status::unavailable;
        }
        if (!current.has_value()) {
            return status::stale_target;
        }
        const bool foreground_current =
            operation.focus == reader_focus::background ||
            (foreground_guard != nullptr && operation.epoch.has_value() &&
                foreground_guard->matches_foreground_epoch(*operation.epoch));
        if (!foreground_current) {
            return status::not_admitted;
        }
        entry.closing = false;
        return status::ok;
    } catch (...) {
        return status::stale_target;
    }
}

result<reader_handle> service::implementation::open_reader(const connection_target target,
    const reader_focus mode) {
    auto prepared = prepare_open(target, mode);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    auto operation = *prepared;

    auto opened = open_native(operation);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }

    status committed{};
    if (operation.focus == reader_focus::foreground_process) {
        const auto guard = focus_observer.lock_presentation();
        committed = commit_open(operation, &guard);
    } else {
        committed = commit_open(operation, nullptr);
    }

    if (committed != status::ok) {
        static_cast<void>(close_native_reader(operation.slot));
        return std::unexpected(committed);
    }
    return reader_handle{.runtime = identity, .id = operation.native.value};
}

status service::implementation::close_reader(const reader_handle handle) {
    if (handle.id == 0U) {
        return status::invalid_argument;
    }
    if (handle.runtime != identity) {
        return status::stale_reader;
    }

    std::size_t index{};
    {
        std::scoped_lock lock{reader_gate};
        const auto found = find_reader_locked(handle.id);
        if (!found.has_value()) {
            return status::stale_reader;
        }
        index = *found;
        auto& entry = readers[index];
        if (entry.close_completed) {
            return status::stale_reader;
        }
        if (entry.open_inflight || entry.close_inflight) {
            return status::busy;
        }
        entry.closing = true;
        entry.pending.reset();
    }

    return close_native_reader(index);
}

} // namespace inputnext::core
