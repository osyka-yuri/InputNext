#include "core_state.hpp"

#include "core.hpp"
#include "history_status.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext::core {
namespace {

[[nodiscard]] history_batch empty_batch(const connection_target target,
    const std::uint64_t stream = 0U) noexcept {
    history_batch batch{};
    batch.domain = history_domain{.target = target, .stream = stream};
    return batch;
}

[[nodiscard]] bool valid_batch(const history_batch& batch,
    const connection_target target,
    const std::uint64_t stream) noexcept {
    const bool stream_is_valid =
        batch.domain.stream != 0U || (batch.count == 0U && batch.failure.has_value());
    const bool stream_matches =
        stream == 0U || batch.domain.stream == stream ||
        (batch.domain.stream == 0U && batch.count == 0U && batch.failure.has_value());
    return batch.domain.target == target && stream_is_valid && stream_matches &&
           batch.count <= history_batch_capacity;
}

[[nodiscard]] status outcome_for(const history_batch& batch) noexcept {
    if (batch.failure.has_value()) {
        return detail::map_history_failure(batch.failure->code);
    }
    return batch.count == 0U ? status::no_data : status::ok;
}

} // namespace

result<service::implementation::read_operation>
service::implementation::begin_read(const reader_handle handle, const std::size_t capacity) {
    if (capacity == 0U || capacity > history_batch_capacity || handle.id == 0U) {
        return std::unexpected(status::invalid_argument);
    }
    if (handle.runtime != identity) {
        return std::unexpected(status::stale_reader);
    }
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    std::scoped_lock lock{reader_gate};
    const auto index = find_reader_locked(handle.id);
    if (!index.has_value()) {
        return std::unexpected(status::stale_reader);
    }
    auto& entry = readers[*index];
    if (entry.closing || entry.close_completed) {
        return std::unexpected(status::stale_reader);
    }
    if (entry.read_inflight || entry.open_inflight) {
        return std::unexpected(status::busy);
    }
    if (entry.terminal != status::ok) {
        return std::unexpected(entry.terminal);
    }

    entry.read_inflight = true;
    return read_operation{.handle = handle,
        .index = *index,
        .capacity = capacity,
        .target = entry.native.target,
        .native = entry.native,
        .focus = entry.focus,
        .pending = entry.pending,
        .stream = entry.stream};
}

status service::implementation::qualify_read(read_operation& operation) const {
    if (operation.focus == reader_focus::foreground_process) {
        const auto guard = focus_observer.lock_presentation();
        operation.epoch = guard.current_foreground_epoch();
        if (!operation.epoch.has_value()) {
            return status::not_admitted;
        }
    }

    auto target = capture_read_target(operation);
    return target.has_value() ? status::ok : target.error();
}

result<history_batch> service::implementation::obtain_batch(read_operation& operation) {
    if (operation.pending.has_value()) {
        return *operation.pending;
    }

    history_batch batch{};
    try {
        batch = runtime.read_history(operation.native, history_time::clock::now());
    } catch (const std::bad_alloc&) {
        batch = empty_batch(operation.target, operation.stream);
        batch.failure = history_failure{.code = history_error::capacity_exceeded};
    } catch (...) {
        batch = empty_batch(operation.target, operation.stream);
        batch.failure = history_failure{.code = history_error::provider_failure};
    }

    if (batch.domain.stream == 0U && batch.count == 0U && batch.failure.has_value() &&
        operation.stream != 0U) {
        batch.domain.stream = operation.stream;
    }

    std::scoped_lock lock{reader_gate};
    auto& entry = readers[operation.index];
    if (!entry.occupied || entry.native.value != operation.handle.id || entry.closing) {
        if (entry.occupied && entry.native.value == operation.handle.id) {
            entry.read_inflight = false;
            release_reader_if_drained_locked(entry);
        }
        return std::unexpected(status::stale_reader);
    }
    entry.pending = batch;
    return batch;
}

result<read_pin> service::implementation::capture_read_target(
    const read_operation& operation) const {
    try {
        auto pin = registry.capture(operation.target);
        if (!pin.has_value()) {
            return std::unexpected(status::source_retired);
        }
        return std::move(*pin);
    } catch (const std::bad_alloc&) {
        return std::unexpected(status::capacity_exceeded);
    } catch (...) {
        return std::unexpected(status::io_failed);
    }
}

reader_result service::implementation::finish_read_without_delivery(const read_operation& operation,
    const status outcome,
    const bool retire_source) {
    reader_result result{.outcome = outcome};
    std::scoped_lock lock{reader_gate};
    if (operation.index >= readers.size() || !readers[operation.index].occupied ||
        readers[operation.index].native.value != operation.handle.id) {
        return reader_result{.outcome = status::stale_reader};
    }

    auto& entry = readers[operation.index];
    if (retire_source) {
        entry.terminal = status::source_retired;
        entry.pending.reset();
    }
    entry.read_inflight = false;
    if (entry.closing || entry.close_completed) {
        result.outcome = status::stale_reader;
        entry.pending.reset();
    }
    result.batch = empty_batch(entry.native.target, entry.stream);
    release_reader_if_drained_locked(entry);
    return result;
}

reader_result service::implementation::commit_read(read_operation& operation, history_batch batch) {
    if (operation.focus == reader_focus::foreground_process) {
        const auto guard = focus_observer.lock_presentation();
        auto target = capture_read_target(operation);
        if (!target.has_value()) {
            const auto failure = target.error();
            return finish_read_without_delivery(operation,
                failure,
                failure == status::source_retired);
        }
        return commit_read_state(operation, batch, &guard);
    }

    auto target = capture_read_target(operation);
    if (!target.has_value()) {
        const auto failure = target.error();
        return finish_read_without_delivery(operation, failure, failure == status::source_retired);
    }
    return commit_read_state(operation, batch, nullptr);
}

reader_result service::implementation::commit_read_state(read_operation& operation,
    history_batch batch,
    const focus::observer::presentation_guard* foreground_guard) {
    std::scoped_lock lock{reader_gate};
    const bool foreground_current =
        operation.focus == reader_focus::background ||
        (foreground_guard != nullptr && operation.epoch.has_value() &&
            foreground_guard->matches_foreground_epoch(*operation.epoch));
    if (operation.index >= readers.size() || !readers[operation.index].occupied ||
        readers[operation.index].native.value != operation.handle.id) {
        return reader_result{.outcome = status::stale_reader};
    }

    auto& entry = readers[operation.index];
    reader_result result{};
    result.batch = empty_batch(operation.target, entry.stream);
    if (entry.closing || entry.close_completed) {
        entry.pending.reset();
        entry.read_inflight = false;
        result.outcome = status::stale_reader;
    } else if (admission_closed.load(std::memory_order_acquire)) {
        entry.read_inflight = false;
        result.outcome = status::unavailable;
    } else if (!foreground_current) {
        entry.read_inflight = false;
        result.outcome = status::not_admitted;
    } else if (entry.terminal != status::ok) {
        entry.read_inflight = false;
        result.outcome = entry.terminal;
    } else if (!valid_batch(batch, operation.target, entry.stream)) {
        entry.terminal = status::io_failed;
        entry.pending.reset();
        entry.read_inflight = false;
        result.outcome = status::io_failed;
    } else {
        if (entry.stream == 0U && batch.domain.stream != 0U) {
            entry.stream = batch.domain.stream;
        }
        entry.read_inflight = false;
        if (batch.count > operation.capacity) {
            result.outcome = status::buffer_too_small;
            result.batch = batch;
        } else {
            result.outcome = outcome_for(batch);
            result.batch = batch;
            entry.pending.reset();
        }
    }

    release_reader_if_drained_locked(entry);
    return result;
}

reader_result service::implementation::read_events(const reader_handle handle,
    const std::size_t capacity) {
    auto operation_result = begin_read(handle, capacity);
    if (!operation_result.has_value()) {
        return reader_result{.outcome = operation_result.error()};
    }
    auto operation = *operation_result;

    const auto qualification = qualify_read(operation);
    if (qualification != status::ok) {
        return finish_read_without_delivery(operation,
            qualification,
            qualification == status::source_retired);
    }

    auto batch = obtain_batch(operation);
    if (!batch.has_value()) {
        return reader_result{.outcome = batch.error()};
    }
    if (batch->failure.has_value() && batch->failure->code == history_error::disconnected) {
        return finish_read_without_delivery(operation, status::source_retired, true);
    }

    return commit_read(operation, *batch);
}

} // namespace inputnext::core
