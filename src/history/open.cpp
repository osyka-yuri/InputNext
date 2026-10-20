#include "store.hpp"

#include "cursor.hpp"
#include "ring.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace inputnext::history_detail {
namespace {

[[nodiscard]] history_failure failure(history_error code) noexcept {
    return history_failure{.code = code};
}

struct open_reservation final {
    std::size_t reader_index{};
    history_reader_handle handle{};
    std::optional<std::size_t> existing_bank;
    std::optional<std::size_t> opening_bank;
};

struct open_selection final {
    history_failure error{.code = history_error::no_data};
    history_sample baseline{};
    history_source_kind source{history_source_kind::callback_capture};
    std::uint64_t next_position{};
    ring_loss_mark loss_mark{};
    bool baseline_ready{};
    bool can_open{true};
};

class open_call_guard final {
public:
    explicit open_call_guard(history_store_state& state) noexcept : state_(state) {}

    ~open_call_guard() noexcept {
        std::scoped_lock lock{state_.gate};
        if (state_.active_open_calls != 0U) {
            --state_.active_open_calls;
        }
        state_.operations_drained.notify_all();
    }

    open_call_guard(const open_call_guard&) = delete;
    open_call_guard& operator=(const open_call_guard&) = delete;
    open_call_guard(open_call_guard&&) = delete;
    open_call_guard& operator=(open_call_guard&&) = delete;

private:
    history_store_state& state_;
};

void abandon_open_locked(history_store_state& state,
    std::size_t reader_index,
    history_reader_handle handle) noexcept {
    auto& reader = state.readers[reader_index];
    if (reader.used && reader.handle == handle) {
        release_reader_bank_reference_locked(state, reader);
        clear_reader(reader);
    }
}

void release_opening_bank_locked(history_store_state& state,
    const open_reservation& reservation) noexcept {
    if (!reservation.opening_bank.has_value() || *reservation.opening_bank >= state.banks.size()) {
        return;
    }

    auto& slot = state.banks[*reservation.opening_bank];
    if (slot.state != bank_slot_state::opening || slot.bank != nullptr ||
        slot.opening_owner != reservation.handle) {
        return;
    }

    slot.opening_owner = {};
    slot.state = bank_slot_state::vacant;
}

[[nodiscard]] history_failure open_error_locked(const history_store_state& state,
    std::size_t reader_index,
    history_reader_handle handle,
    history_failure fallback) noexcept {
    if (state.closed) {
        return failure(history_error::closed);
    }

    const auto& reader = state.readers[reader_index];
    if (!reader.used || reader.handle != handle) {
        return fallback;
    }
    if (reader.closing) {
        return failure(history_error::closed);
    }
    if (reader.terminal) {
        return reader.terminal_failure;
    }
    return fallback;
}

[[nodiscard]] history_failure abandon_open(history_store_state& state,
    const open_reservation& reservation,
    history_failure fallback) noexcept {
    std::scoped_lock lock{state.gate};
    const auto result =
        open_error_locked(state, reservation.reader_index, reservation.handle, fallback);
    release_opening_bank_locked(state, reservation);
    abandon_open_locked(state, reservation.reader_index, reservation.handle);
    state.operations_drained.notify_all();
    return result;
}

[[nodiscard]] history_result<open_reservation> reserve_open(history_store_state& state,
    connection_target target,
    std::optional<std::uint32_t> reserved_position) noexcept {
    std::scoped_lock lock{state.gate};
    if (state.closed) {
        return std::unexpected(failure(history_error::closed));
    }

    auto reservation = open_reservation{};
    if (reserved_position.has_value()) {
        const auto reader_index =
            public_history_reader_capacity + static_cast<std::size_t>(*reserved_position);
        if (reader_index >= state.readers.size()) {
            return std::unexpected(failure(history_error::invalid_argument));
        }
        if (state.readers[reader_index].used) {
            return std::unexpected(failure(history_error::busy));
        }
    }

    if (find_opening_bank_locked(state, target).has_value()) {
        return std::unexpected(failure(history_error::busy));
    }

    const auto allocated =
        reserved_position.has_value()
            ? allocate_xinput_reader_locked(state,
                  *reserved_position,
                  target,
                  reservation.reader_index,
                  reservation.handle)
            : allocate_reader_locked(state, target, reservation.reader_index, reservation.handle);
    if (!allocated) {
        const auto identities_exhausted = state.next_reader_id == 0U || state.next_stream_id == 0U;
        return std::unexpected(failure(identities_exhausted ? history_error::identity_exhausted
                                                            : history_error::capacity_exceeded));
    }

    reservation.existing_bank = find_active_bank_locked(state, target);
    if (reservation.existing_bank.has_value()) {
        reserve_bank_operation_locked(state,
            *reservation.existing_bank,
            state.readers[reservation.reader_index]);
    } else {
        const auto vacant_bank = find_vacant_bank_locked(state);
        if (!vacant_bank.has_value()) {
            abandon_open_locked(state, reservation.reader_index, reservation.handle);
            --state.active_open_calls;
            state.operations_drained.notify_all();
            return std::unexpected(failure(history_error::capacity_exceeded));
        }

        auto& slot = state.banks[*vacant_bank];
        slot.opening_owner = reservation.handle;
        slot.state = bank_slot_state::opening;
        reservation.opening_bank = *vacant_bank;
    }
    return reservation;
}

[[nodiscard]] history_result<std::unique_ptr<history_cursor>>
create_cursor(history_provider& provider, connection_target target) noexcept {
    try {
        return provider.open_cursor(target);
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(history_error::capacity_exceeded));
    } catch (...) {
        return std::unexpected(failure(history_error::provider_failure));
    }
}

[[nodiscard]] history_result<std::unique_ptr<history_bank>> make_bank(connection_target target,
    std::unique_ptr<history_cursor> cursor) noexcept {
    try {
        return std::make_unique<history_bank>(target, std::move(cursor));
    } catch (const std::bad_alloc&) {
        return std::unexpected(failure(history_error::capacity_exceeded));
    } catch (...) {
        return std::unexpected(failure(history_error::provider_failure));
    }
}

[[nodiscard]] history_result<std::size_t> admit_candidate(history_store_state& state,
    const open_reservation& reservation,
    std::unique_ptr<history_bank>& candidate) noexcept {
    std::scoped_lock lock{state.gate};
    auto& reader = state.readers[reservation.reader_index];
    if (!reservation.opening_bank.has_value() || *reservation.opening_bank >= state.banks.size()) {
        const auto result = open_error_locked(state,
            reservation.reader_index,
            reservation.handle,
            failure(history_error::provider_failure));
        return std::unexpected(result);
    }

    auto& slot = state.banks[*reservation.opening_bank];
    const auto owns_opening_slot = slot.state == bank_slot_state::opening && slot.bank == nullptr &&
                                   slot.opening_owner == reservation.handle;
    if (state.closed || !reader.used || reader.handle != reservation.handle || !reader.opening ||
        reader.cancelled || !owns_opening_slot) {
        const auto result = open_error_locked(state,
            reservation.reader_index,
            reservation.handle,
            failure(history_error::closed));
        return std::unexpected(result);
    }

    if (find_active_bank_locked(state, reservation.handle.target).has_value()) {
        return std::unexpected(failure(history_error::busy));
    }

    slot.bank = std::move(candidate);
    slot.opening_owner = {};
    slot.state = bank_slot_state::active;
    reserve_bank_operation_locked(state, *reservation.opening_bank, reader);
    return *reservation.opening_bank;
}

[[nodiscard]] history_bank* find_reserved_bank(history_store_state& state,
    const open_reservation& reservation,
    std::size_t bank_index) noexcept {
    std::scoped_lock lock{state.gate};
    const auto& reader = state.readers[reservation.reader_index];
    const auto& slot = state.banks[bank_index];
    if (!reader.used || reader.handle != reservation.handle || !reader.opening ||
        !reader.bank_attached || reader.bank_index != bank_index || slot.bank == nullptr) {
        return nullptr;
    }
    return slot.bank.get();
}

void select_open_baseline(history_bank const& bank, open_selection& selection) noexcept {
    selection.next_position = bank.ring.next_position();
    selection.loss_mark = bank.ring.tail_loss_mark();
    if (bank.terminal) {
        selection.error = bank.deferred_failure;
        selection.can_open = false;
        return;
    }
    if (!bank.started || bank.recovery_pending) {
        return;
    }

    const auto* latest = bank.ring.latest();
    if (latest == nullptr) {
        selection.error = failure(history_error::provider_failure);
        selection.can_open = false;
        return;
    }

    selection.baseline = latest->sample;
    selection.source = latest->source;
    selection.baseline_ready = true;
}

[[nodiscard]] history_failure completion_error_locked(const history_store_state& state,
    const history_reader_state& reader,
    const history_bank_slot& slot,
    history_failure fallback) noexcept {
    if (state.closed || reader.closing) {
        return failure(history_error::closed);
    }
    if (reader.terminal) {
        return reader.terminal_failure;
    }
    if (reader.cancelled || slot.state != bank_slot_state::active) {
        return failure(history_error::disconnected);
    }
    return fallback;
}

[[nodiscard]] history_result<history_reader_handle> complete_open(history_store_state& state,
    const open_reservation& reservation,
    std::size_t bank_index,
    const open_selection& selection) noexcept {
    std::scoped_lock lock{state.gate};
    auto const& slot = state.banks[bank_index];
    auto& reader = state.readers[reservation.reader_index];
    const auto valid_reservation =
        reader.used && reader.handle == reservation.handle && reader.opening && !reader.cancelled &&
        !reader.terminal && !state.closed && slot.state == bank_slot_state::active &&
        slot.bank != nullptr && slot.bank->target == reservation.handle.target;
    if (valid_reservation && selection.can_open) {
        reader.opening = false;
        reader.next_position = selection.next_position;
        reader.loss_mark = selection.loss_mark;
        reader.selected_baseline_ready = selection.baseline_ready;
        if (selection.baseline_ready) {
            reader.selected_baseline = selection.baseline;
            reader.selected_source = selection.source;
        }
    } else {
        const auto error = completion_error_locked(state, reader, slot, selection.error);
        if (reader.used && reader.handle == reservation.handle) {
            release_reader_bank_reference_locked(state, reader);
            clear_reader(reader);
        }
        if (slot.bank != nullptr && slot.bank->active_operations != 0U) {
            --slot.bank->active_operations;
        }
        state.operations_drained.notify_all();
        return std::unexpected(error);
    }

    if (slot.bank != nullptr && slot.bank->active_operations != 0U) {
        --slot.bank->active_operations;
    }
    state.operations_drained.notify_all();
    return reservation.handle;
}

[[nodiscard]] history_result<history_reader_handle> finish_open(history_store_state& state,
    const open_reservation& reservation,
    std::size_t bank_index) {
    auto selection = open_selection{};
    auto* bank = find_reserved_bank(state, reservation, bank_index);
    if (bank == nullptr) {
        selection.error = failure(history_error::closed);
        selection.can_open = false;
        return complete_open(state, reservation, bank_index, selection);
    }

    if (!state.provider.is_live(reservation.handle.target)) {
        retire_target(state, reservation.handle.target);
        selection.error = failure(history_error::disconnected);
        selection.can_open = false;
        return complete_open(state, reservation, bank_index, selection);
    }

    std::unique_lock operation_lock{bank->operation_gate, std::try_to_lock};
    if (!operation_lock.owns_lock()) {
        selection.error = failure(history_error::busy);
        selection.can_open = false;
        return complete_open(state, reservation, bank_index, selection);
    }

    select_open_baseline(*bank, selection);
    operation_lock.unlock();

    if (!state.provider.is_live(reservation.handle.target)) {
        retire_target(state, reservation.handle.target);
        selection.error = failure(history_error::disconnected);
        selection.baseline_ready = false;
        selection.can_open = false;
    }
    return complete_open(state, reservation, bank_index, selection);
}

} // namespace

namespace {

[[nodiscard]] history_result<history_reader_handle> open_reader(history_store_state& state,
    connection_target target,
    [[maybe_unused]] history_time now,
    std::optional<std::uint32_t> reserved_position) {
    if (!target.valid()) {
        return std::unexpected(failure(history_error::invalid_argument));
    }
    if (reserved_position.has_value() &&
        *reserved_position >= reserved_xinput_history_reader_capacity) {
        return std::unexpected(failure(history_error::invalid_argument));
    }

    auto reserved = reserve_open(state, target, reserved_position);
    if (!reserved.has_value()) {
        return std::unexpected(reserved.error());
    }
    const auto reservation = *reserved;
    [[maybe_unused]] const auto open_guard = open_call_guard{state};

    if (reservation.existing_bank.has_value()) {
        return finish_open(state, reservation, *reservation.existing_bank);
    }

    auto cursor = create_cursor(state.provider, target);
    if (!cursor.has_value() || cursor.value() == nullptr) {
        const auto cursor_error =
            cursor.has_value() ? failure(history_error::capacity_exceeded) : cursor.error();
        return std::unexpected(abandon_open(state, reservation, cursor_error));
    }

    auto candidate = make_bank(target, std::move(cursor.value()));
    if (!candidate.has_value()) {
        return std::unexpected(abandon_open(state, reservation, candidate.error()));
    }

    auto bank_index = admit_candidate(state, reservation, *candidate);
    if (!bank_index.has_value()) {
        candidate->reset();
        return std::unexpected(abandon_open(state, reservation, bank_index.error()));
    }
    candidate->reset();
    return finish_open(state, reservation, *bank_index);
}

} // namespace

history_result<history_reader_handle> history_store::open(connection_target target,
    [[maybe_unused]] history_time now) {
    return open_reader(state_, target, now, std::nullopt);
}

history_result<history_reader_handle> history_store::open_xinput_reader(std::uint32_t position,
    connection_target target,
    history_time now) {
    return open_reader(state_, target, now, position);
}

} // namespace inputnext::history_detail
