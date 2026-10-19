#include "store.hpp"

#include "cursor.hpp"
#include "ring.hpp"
#include "state.hpp"
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace inputnext::history_detail {
namespace {

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

enum class ring_read_state : std::uint8_t { empty, progressed, stopped };
enum class cursor_start_state : std::uint8_t { not_needed, started, failed };
enum class read_execution_state : std::uint8_t { completed, bank_busy, source_disconnected };

struct read_operation final {
    history_reader_handle handle{};
    history_batch batch{};
    history_sample selected_baseline{};
    history_source_kind selected_source{history_source_kind::callback_capture};
    history_failure terminal_failure{.code = history_error::sequence_exhausted};
    std::size_t reader_index{};
    std::size_t bank_index{};
    std::size_t remaining_work{history_work_per_read};
    std::uint64_t stream_id{};
    std::uint64_t next_stream_id{};
    std::uint64_t next_position{};
    std::uint64_t next_output_sequence{};
    ring_loss_mark loss_mark{};
    void (*before_commit_test_hook)(void*) noexcept {};
    void* before_commit_test_context{};
    bool selected_baseline_ready{};
    bool terminal{};
};

[[nodiscard]] history_failure failure(history_error code) noexcept {
    return history_failure{.code = code};
}

[[nodiscard]] history_failure failure(history_error code, std::uint32_t native_code) noexcept {
    return history_failure{.code = code, .native_code = native_code};
}

[[nodiscard]] history_batch
failed_batch(history_reader_handle reader, std::uint64_t stream, history_failure error) noexcept {
    auto batch = history_batch{};
    batch.domain = history_domain{.target = reader.target, .stream = stream};
    batch.failure = error;
    return batch;
}

void append_record(history_batch& batch,
    const history_sample& sample,
    history_source_kind source,
    std::uint64_t sequence,
    bool baseline,
    history_record_origin origin = history_record_origin::baseline) noexcept {
    auto& record = batch.records[batch.count];
    record = history_record{.sample = sample,
        .sequence = sequence,
        .baseline = baseline,
        .origin = baseline ? history_record_origin::baseline : origin};
    if (batch.count == 0U) {
        batch.first_sequence = sequence;
        batch.source = source;
    }
    batch.last_sequence = sequence;
    ++batch.count;
}

[[nodiscard]] bool reserve_read(history_store_state& state,
    history_reader_handle reader_handle,
    read_operation& operation) noexcept {
    operation.handle = reader_handle;
    if (!reader_handle.target.valid() || reader_handle.value == 0U) {
        operation.batch = failed_batch(reader_handle, 0U, failure(history_error::invalid_argument));
        return false;
    }

    std::scoped_lock lock{state.gate};
    if (state.closed) {
        operation.batch = failed_batch(reader_handle, 0U, failure(history_error::closed));
        return false;
    }

    const auto reader_index = find_reader_locked(state, reader_handle);
    if (!reader_index.has_value()) {
        operation.batch = failed_batch(reader_handle, 0U, failure(history_error::invalid_reader));
        return false;
    }

    auto& reader = state.readers[*reader_index];
    if (reader.opening || reader.closing) {
        operation.batch =
            failed_batch(reader_handle, reader.stream_id, failure(history_error::closed));
        return false;
    }
    if (reader.terminal) {
        operation.batch = failed_batch(reader_handle, reader.stream_id, reader.terminal_failure);
        return false;
    }
    if (reader.operation_busy) {
        operation.batch =
            failed_batch(reader_handle, reader.stream_id, failure(history_error::busy));
        return false;
    }
    if (!reader.bank_attached || reader.bank_index >= state.banks.size() ||
        state.banks[reader.bank_index].bank == nullptr) {
        operation.batch =
            failed_batch(reader_handle, reader.stream_id, failure(history_error::disconnected));
        return false;
    }
    if (state.active_read_calls == std::numeric_limits<std::size_t>::max()) {
        operation.batch = failed_batch(reader_handle,
            reader.stream_id,
            failure(history_error::capacity_exceeded));
        return false;
    }

    operation.reader_index = *reader_index;
    operation.bank_index = reader.bank_index;
    operation.stream_id = reader.stream_id;
    operation.next_stream_id = reader.stream_id;
    operation.next_position = reader.next_position;
    operation.next_output_sequence = reader.next_output_sequence;
    operation.loss_mark = reader.loss_mark;
    operation.selected_baseline = reader.selected_baseline;
    operation.selected_source = reader.selected_source;
    operation.selected_baseline_ready = reader.selected_baseline_ready;
    operation.before_commit_test_hook = state.before_read_commit_test_hook;
    operation.before_commit_test_context = state.before_read_commit_test_context;
    operation.batch = failed_batch(reader_handle, reader.stream_id, failure(history_error::busy));
    reader.operation_busy = true;
    ++state.banks[operation.bank_index].bank->active_operations;
    ++state.active_read_calls;
    return true;
}

void clear_batch_records(history_batch& batch) noexcept {
    batch.records = {};
    batch.count = 0U;
    batch.first_sequence = 0U;
    batch.last_sequence = 0U;
    batch.checkpoint_sequence = 0U;
    batch.checkpoint_qpc = 0U;
    batch.caught_up = false;
    batch.reset = history_reset::none;
    batch.loss = history_loss::none;
    batch.lost_count = 0U;
}

[[nodiscard]] history_failure cancelled_read_failure(const history_store_state& state,
    const history_reader_state& reader) noexcept {
    if (state.closed || reader.closing) {
        return failure(history_error::closed);
    }
    if (reader.terminal) {
        return reader.terminal_failure;
    }
    return failure(history_error::disconnected);
}

void commit_read(history_store_state& state, read_operation& operation, bool source_live) noexcept {
    std::scoped_lock lock{state.gate};
    auto const& slot = state.banks[operation.bank_index];
    auto& reader = state.readers[operation.reader_index];
    const auto same_reader = reader.used && reader.handle == operation.handle;
    const auto can_commit = source_live && same_reader && reader.operation_busy &&
                            reader.bank_attached && reader.bank_index == operation.bank_index &&
                            !reader.closing && !reader.terminal && !state.closed &&
                            slot.state == bank_slot_state::active && slot.bank != nullptr &&
                            slot.bank->target == operation.handle.target;

    if (can_commit) {
        reader.next_position = operation.next_position;
        reader.loss_mark = operation.loss_mark;
        reader.stream_id = operation.next_stream_id;
        reader.next_output_sequence = operation.next_output_sequence;
        reader.selected_baseline_ready = operation.selected_baseline_ready;
        if (operation.terminal) {
            reader.terminal = true;
            reader.terminal_failure = operation.terminal_failure;
        }
    } else {
        clear_batch_records(operation.batch);
        operation.batch.domain = history_domain{
            .target = operation.handle.target,
            .stream = operation.next_stream_id,
        };
        operation.batch.failure = same_reader ? cancelled_read_failure(state, reader)
                                              : failure(history_error::disconnected);
    }

    if (same_reader) {
        reader.operation_busy = false;
        if (reader.closing) {
            release_reader_bank_reference_locked(state, reader);
            clear_reader(reader);
        }
    }
    if (slot.bank != nullptr && slot.bank->active_operations != 0U) {
        --slot.bank->active_operations;
    }
    if (state.active_read_calls != 0U) {
        --state.active_read_calls;
    }
    state.operations_drained.notify_all();
}

[[nodiscard]] std::optional<std::uint64_t> reserve_stream_id(history_store_state& state,
    const read_operation& operation) noexcept {
    std::scoped_lock lock{state.gate};
    const auto reader_index = find_reader_locked(state, operation.handle);
    if (!reader_index.has_value()) {
        return std::nullopt;
    }

    const auto& reader = state.readers[*reader_index];
    if (state.closed || reader.closing || reader.terminal || reader.opening ||
        state.next_stream_id == 0U) {
        return std::nullopt;
    }

    const auto stream_id = state.next_stream_id;
    state.next_stream_id = stream_id == maximum ? 0U : stream_id + 1U;
    return stream_id;
}

void set_bank_failure(history_bank& bank, history_failure error) noexcept {
    bank.terminal = true;
    bank.deferred_failure = error;
    bank.checkpoint_position = 0U;
    bank.checkpoint_qpc = 0U;
}

[[nodiscard]] bool append_seed(history_bank& bank,
    const cursor_seed& seed,
    ring_entry_kind kind,
    history_loss loss,
    std::uint64_t lost_count) noexcept {
    const auto entry = ring_entry{
        .sample = seed.baseline,
        .source = seed.source,
        .kind = kind,
        .loss = loss,
        .lost_count = lost_count,
    };
    std::uint64_t position{};
    if (!bank.ring.append(entry, position)) {
        return false;
    }

    bank.started = true;
    bank.qpc_frequency = seed.qpc_frequency;
    bank.last_qpc = seed.baseline.effective_qpc;
    bank.source = seed.source;
    bank.checkpoint_position = 0U;
    bank.checkpoint_qpc = 0U;
    return true;
}

[[nodiscard]] bool valid_seed(const cursor_seed& seed) noexcept {
    return seed.qpc_frequency != 0U && seed.baseline.effective_qpc != 0U &&
           is_valid_gamepad_state(seed.baseline.state);
}

void terminal_reader(read_operation& operation, history_failure error) noexcept {
    operation.batch.failure = error;
    operation.terminal = true;
    operation.terminal_failure = error;
}

void append_history_gap(history_store_state& state,
    history_bank const& bank,
    read_operation& operation,
    ring_overrun overrun) noexcept {
    const auto* latest = bank.ring.latest();
    if (latest == nullptr) {
        operation.batch.failure = failure(history_error::no_data);
        return;
    }

    const auto stream_id = reserve_stream_id(state, operation);
    if (!stream_id.has_value()) {
        terminal_reader(operation, failure(history_error::identity_exhausted));
        return;
    }

    operation.next_stream_id = *stream_id;
    operation.next_output_sequence = 2U;
    operation.next_position = bank.ring.next_position();
    operation.batch.domain.stream = *stream_id;
    operation.batch.reset = history_reset::history_gap;
    operation.batch.loss = overrun.loss;
    operation.batch.lost_count = overrun.lost_count;
    operation.loss_mark = bank.ring.tail_loss_mark();
    append_record(operation.batch, latest->sample, latest->source, 1U, true);
}

void append_baseline_cut(history_store_state& state,
    const ring_entry& entry,
    read_operation& operation) noexcept {
    if (entry.kind == ring_entry_kind::recovery_baseline) {
        const auto stream_id = reserve_stream_id(state, operation);
        if (!stream_id.has_value()) {
            terminal_reader(operation, failure(history_error::identity_exhausted));
            return;
        }
        operation.next_stream_id = *stream_id;
        operation.batch.domain.stream = *stream_id;
        operation.batch.reset = history_reset::history_gap;
        operation.batch.loss = entry.loss;
        operation.batch.lost_count = entry.lost_count;
    } else {
        operation.batch.reset = history_reset::initial_open;
    }

    operation.batch.source = entry.source;
    append_record(operation.batch, entry.sample, entry.source, 1U, true);
    operation.next_position = entry.position + 1U;
    operation.loss_mark = entry.loss_mark;
    operation.next_output_sequence = 2U;
}

[[nodiscard]] ring_read_state append_ring_entry(history_store_state& state,
    history_bank& bank,
    read_operation& operation) noexcept {
    if (operation.next_position < bank.ring.oldest_position()) {
        append_history_gap(state,
            bank,
            operation,
            bank.ring.overrun(operation.next_position, operation.loss_mark));
        return ring_read_state::stopped;
    }
    if (operation.next_position >= bank.ring.next_position()) {
        return ring_read_state::empty;
    }

    const auto* entry = bank.ring.find(operation.next_position);
    if (entry == nullptr) {
        operation.batch.failure = failure(history_error::provider_failure);
        return ring_read_state::stopped;
    }
    if (entry->kind != ring_entry_kind::reading) {
        if (operation.batch.count != 0U) {
            return ring_read_state::stopped;
        }
        append_baseline_cut(state, *entry, operation);
        return ring_read_state::stopped;
    }

    if (operation.next_output_sequence == 0U) {
        terminal_reader(operation, failure(history_error::sequence_exhausted));
        return ring_read_state::stopped;
    }

    append_record(operation.batch,
        entry->sample,
        entry->source,
        operation.next_output_sequence,
        false,
        entry->origin);
    ++operation.next_position;
    operation.loss_mark = entry->loss_mark;
    operation.next_output_sequence =
        operation.next_output_sequence == maximum ? 0U : operation.next_output_sequence + 1U;
    if (operation.next_output_sequence == 0U) {
        terminal_reader(operation, failure(history_error::sequence_exhausted));
        return ring_read_state::stopped;
    }
    return ring_read_state::progressed;
}

void append_started_baseline(read_operation& operation) noexcept {
    append_record(operation.batch,
        operation.selected_baseline,
        operation.selected_source,
        1U,
        true);
    operation.batch.reset = history_reset::initial_open;
    operation.selected_baseline_ready = false;
    operation.next_output_sequence = 2U;
}

void recover_bank(history_bank& bank,
    history_failure error,
    history_loss loss,
    std::uint64_t lost_count) noexcept {
    bank.recovery_pending = true;
    bank.recovery_loss = loss;
    bank.recovery_lost_count = lost_count;
    bank.deferred_failure = error;
    bank.checkpoint_position = 0U;
    bank.checkpoint_qpc = 0U;
}

[[nodiscard]] cursor_start_state
start_cursor(history_bank& bank, read_operation& operation, history_time now) noexcept {
    if (bank.started && !bank.recovery_pending) {
        return cursor_start_state::not_needed;
    }

    const auto recovering = bank.started;
    const auto started = bank.cursor->start(now);
    if (!started.has_value()) {
        const auto start_failure = started.error();
        if (start_failure.code == history_error::disconnected ||
            start_failure.code == history_error::sequence_exhausted) {
            set_bank_failure(bank, start_failure);
        } else if (recovering) {
            bank.recovery_pending = true;
            bank.deferred_failure = start_failure;
        }
        operation.batch.failure = start_failure;
        return cursor_start_state::failed;
    }
    if (!valid_seed(*started)) {
        auto invalid = failure(history_error::provider_failure);
        if (started->qpc_frequency == 0U || started->baseline.effective_qpc == 0U) {
            invalid = failure(history_error::clock_discontinuity);
        }
        if (recovering) {
            recover_bank(bank, invalid, history_loss::unknown, 0U);
        }
        operation.batch.failure = invalid;
        return cursor_start_state::failed;
    }

    const auto kind =
        recovering ? ring_entry_kind::recovery_baseline : ring_entry_kind::initial_baseline;
    const auto loss = recovering ? bank.recovery_loss : history_loss::none;
    const auto lost_count = recovering ? bank.recovery_lost_count : 0U;
    if (!append_seed(bank, *started, kind, loss, lost_count)) {
        set_bank_failure(bank, failure(history_error::sequence_exhausted));
        operation.batch.failure = bank.deferred_failure;
        return cursor_start_state::failed;
    }

    bank.recovery_pending = false;
    bank.recovery_loss = history_loss::unknown;
    bank.recovery_lost_count = 0U;
    return cursor_start_state::started;
}

[[nodiscard]] bool append_cursor_reading(history_bank& bank,
    const cursor_step& step,
    read_operation& operation) noexcept {
    if (!is_valid_gamepad_state(step.reading.state) || step.reading.effective_qpc == 0U ||
        (bank.last_qpc != 0U && step.reading.effective_qpc < bank.last_qpc) ||
        (bank.checkpoint_qpc != 0U && step.reading.effective_qpc < bank.checkpoint_qpc)) {
        recover_bank(bank,
            failure(history_error::clock_discontinuity, step.native_code),
            history_loss::unknown,
            0U);
        operation.batch.failure = bank.deferred_failure;
        return false;
    }

    const auto entry = ring_entry{
        .sample = step.reading,
        .source = bank.source,
        .kind = ring_entry_kind::reading,
        .origin = step.origin,
    };
    std::uint64_t position{};
    if (!bank.ring.append(entry, position)) {
        set_bank_failure(bank, failure(history_error::sequence_exhausted));
        operation.batch.failure = bank.deferred_failure;
        return false;
    }
    bank.last_qpc = step.reading.effective_qpc;
    return true;
}

void update_checkpoint(history_bank& bank,
    const cursor_step& step,
    read_operation& operation) noexcept {
    const auto valid_checkpoint =
        step.checkpoint_qpc != 0U &&
        (bank.last_qpc == 0U || step.checkpoint_qpc >= bank.last_qpc) &&
        (bank.checkpoint_qpc == 0U || step.checkpoint_qpc >= bank.checkpoint_qpc);
    if (valid_checkpoint) {
        bank.checkpoint_position = bank.ring.next_position();
        bank.checkpoint_qpc = step.checkpoint_qpc;
        return;
    }

    const auto moved_backwards =
        step.checkpoint_qpc != 0U &&
        ((bank.last_qpc != 0U && step.checkpoint_qpc < bank.last_qpc) ||
            (bank.checkpoint_qpc != 0U && step.checkpoint_qpc < bank.checkpoint_qpc));
    bank.checkpoint_position = 0U;
    bank.checkpoint_qpc = 0U;
    if (moved_backwards) {
        recover_bank(bank,
            failure(history_error::clock_discontinuity, step.native_code),
            history_loss::unknown,
            0U);
        operation.batch.failure = bank.deferred_failure;
    }
}

[[nodiscard]] bool append_cursor_step(history_bank& bank,
    const cursor_step& step,
    read_operation& operation) noexcept {
    switch (step.kind) {
    case cursor_step_kind::reading:
        return append_cursor_reading(bank, step, operation);
    case cursor_step_kind::progress:
        return true;
    case cursor_step_kind::caught_up:
        update_checkpoint(bank, step, operation);
        operation.remaining_work = 0U;
        return false;
    case cursor_step_kind::history_lost: {
        const auto loss = step.loss == history_loss::none ? history_loss::unknown : step.loss;
        const auto error = failure(history_error::history_lost, step.native_code);
        recover_bank(bank, error, loss, step.lost_count);
        operation.batch.failure = error;
        operation.remaining_work = 0U;
        return false;
    }
    case cursor_step_kind::provider_failure:
        operation.batch.failure = failure(history_error::provider_failure, step.native_code);
        operation.remaining_work = 0U;
        return false;
    case cursor_step_kind::clock_discontinuity: {
        const auto error = failure(history_error::clock_discontinuity, step.native_code);
        recover_bank(bank, error, history_loss::unknown, 0U);
        operation.batch.failure = error;
        operation.remaining_work = 0U;
        return false;
    }
    case cursor_step_kind::disconnected:
        set_bank_failure(bank, failure(history_error::disconnected, step.native_code));
        operation.batch.failure = bank.deferred_failure;
        operation.remaining_work = 0U;
        return false;
    case cursor_step_kind::exhausted:
        set_bank_failure(bank, failure(history_error::sequence_exhausted, step.native_code));
        operation.batch.failure = bank.deferred_failure;
        operation.remaining_work = 0U;
        return false;
    }
    return false;
}

void set_caught_up(history_bank const& bank, read_operation& operation) noexcept {
    if (operation.batch.failure.has_value() || bank.checkpoint_position == 0U ||
        bank.checkpoint_position != bank.ring.next_position() ||
        operation.next_position < bank.checkpoint_position) {
        return;
    }

    operation.batch.caught_up = true;
    operation.batch.checkpoint_qpc = bank.checkpoint_qpc;
    if (operation.next_output_sequence == 0U) {
        operation.batch.checkpoint_sequence = maximum;
    } else {
        operation.batch.checkpoint_sequence = operation.next_output_sequence - 1U;
    }
}

void populate_batch(history_store_state& state,
    history_bank& bank,
    read_operation& operation,
    history_time now) noexcept {
    operation.batch.domain = history_domain{
        .target = operation.handle.target,
        .stream = operation.stream_id,
    };
    operation.batch.source = bank.source;
    if (operation.selected_baseline_ready) {
        append_started_baseline(operation);
        return;
    }

    while (operation.remaining_work != 0U && operation.batch.count < history_batch_capacity) {
        const auto ring_state = append_ring_entry(state, bank, operation);
        if (ring_state == ring_read_state::progressed) {
            continue;
        }
        if (ring_state == ring_read_state::stopped) {
            break;
        }
        if (bank.terminal) {
            operation.batch.failure = bank.deferred_failure;
            break;
        }
        if (bank.ring.next_position() == maximum) {
            set_bank_failure(bank, failure(history_error::sequence_exhausted));
            operation.batch.failure = bank.deferred_failure;
            break;
        }

        const auto start_state = start_cursor(bank, operation, now);
        if (start_state == cursor_start_state::started) {
            continue;
        }
        if (start_state == cursor_start_state::failed) {
            break;
        }
        if (bank.ring.next_position() == maximum) {
            set_bank_failure(bank, failure(history_error::sequence_exhausted));
            operation.batch.failure = bank.deferred_failure;
            break;
        }

        --operation.remaining_work;
        const auto step = bank.cursor->next(now);
        if (!append_cursor_step(bank, step, operation)) {
            break;
        }
    }
    set_caught_up(bank, operation);
}

[[nodiscard]] read_execution_state
execute_read(history_store_state& state, read_operation& operation, history_time now) {
    auto* bank = state.banks[operation.bank_index].bank.get();
    std::unique_lock bank_lock{bank->operation_gate, std::try_to_lock};
    if (!bank_lock.owns_lock()) {
        return read_execution_state::bank_busy;
    }
    if (!state.provider.is_live(operation.handle.target)) {
        bank_lock.unlock();
        return read_execution_state::source_disconnected;
    }

    operation.batch.failure.reset();
    populate_batch(state, *bank, operation, now);
    bank_lock.unlock();
    return state.provider.is_live(operation.handle.target)
               ? read_execution_state::completed
               : read_execution_state::source_disconnected;
}

void invoke_before_commit_hook(const read_operation& operation) {
    if (operation.before_commit_test_hook != nullptr) {
        operation.before_commit_test_hook(operation.before_commit_test_context);
    }
}

} // namespace

history_batch history_store::read(history_reader_handle reader, history_time now) {
    auto operation = read_operation{};
    if (!reserve_read(state_, reader, operation)) {
        return operation.batch;
    }

    const auto execution = execute_read(state_, operation, now);
    if (execution == read_execution_state::completed) {
        invoke_before_commit_hook(operation);
    } else if (execution == read_execution_state::source_disconnected) {
        retire(reader.target);
    }
    commit_read(state_, operation, execution != read_execution_state::source_disconnected);
    return operation.batch;
}

} // namespace inputnext::history_detail
