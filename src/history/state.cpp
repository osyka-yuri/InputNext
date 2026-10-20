#include "state.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace inputnext::history_detail {
namespace {

constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();

[[nodiscard]] bool allocate_reader_at_locked(history_store_state& state,
    std::size_t index,
    connection_target target,
    std::size_t& reader_index,
    history_reader_handle& handle) noexcept {
    if (index >= state.readers.size() || state.readers[index].used || state.next_reader_id == 0U ||
        state.next_stream_id == 0U ||
        state.active_open_calls == std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    reader_index = index;
    const auto reader_id = state.next_reader_id;
    const auto stream_id = state.next_stream_id;
    state.next_reader_id = reader_id == maximum ? 0U : reader_id + 1U;
    state.next_stream_id = stream_id == maximum ? 0U : stream_id + 1U;
    handle = history_reader_handle{.target = target, .value = reader_id};

    auto& reader = state.readers[index];
    reader = history_reader_state{
        .handle = handle,
        .bank_index = invalid_bank_index,
        .stream_id = stream_id,
        .next_output_sequence = 1U,
        .terminal_failure = history_failure{.code = history_error::closed},
        .used = true,
        .opening = true,
    };
    ++state.active_open_calls;
    return true;
}

} // namespace

std::optional<std::size_t> find_reader_locked(const history_store_state& state,
    history_reader_handle handle) noexcept {
    for (std::size_t index = 0U; index < state.readers.size(); ++index) {
        const auto& reader = state.readers[index];
        if (reader.used && reader.handle == handle) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_active_bank_locked(const history_store_state& state,
    connection_target target) noexcept {
    for (std::size_t index = 0U; index < state.banks.size(); ++index) {
        const auto& slot = state.banks[index];
        if (slot.state == bank_slot_state::active && slot.bank != nullptr &&
            slot.bank->target == target) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_opening_bank_locked(const history_store_state& state,
    connection_target target) noexcept {
    for (std::size_t index = 0U; index < state.banks.size(); ++index) {
        const auto& slot = state.banks[index];
        if (slot.state == bank_slot_state::opening && slot.opening_owner.target == target) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_vacant_bank_locked(const history_store_state& state) noexcept {
    for (std::size_t index = 0U; index < state.banks.size(); ++index) {
        if (state.banks[index].state == bank_slot_state::vacant) {
            return index;
        }
    }
    return std::nullopt;
}

void release_reader_bank_reference_locked(history_store_state& state,
    history_reader_state& reader) noexcept {
    if (!reader.bank_attached) {
        return;
    }

    auto const& slot = state.banks[reader.bank_index];
    if (slot.bank != nullptr && slot.bank->reader_references != 0U) {
        --slot.bank->reader_references;
    }
    reader.bank_attached = false;
}

void clear_reader(history_reader_state& reader) noexcept {
    reader = history_reader_state{};
    reader.bank_index = invalid_bank_index;
}

void retire_target_locked(history_store_state& state, connection_target target) noexcept {
    for (auto& reader : state.readers) {
        if (!reader.used || reader.handle.target != target) {
            continue;
        }
        reader.terminal = true;
        reader.cancelled = reader.opening;
        reader.terminal_failure = history_failure{.code = history_error::disconnected};
        release_reader_bank_reference_locked(state, reader);
        if (!reader.operation_busy && !reader.opening && reader.closing) {
            clear_reader(reader);
        }
    }

    for (auto& slot : state.banks) {
        if (slot.state != bank_slot_state::active || slot.bank == nullptr ||
            slot.bank->target != target) {
            continue;
        }
        slot.state = bank_slot_state::retiring;
    }
}

void retire_target(history_store_state& state, connection_target target) noexcept {
    std::scoped_lock lock{state.gate};
    retire_target_locked(state, target);
    state.operations_drained.notify_all();
}

void reserve_bank_operation_locked(history_store_state& state,
    std::size_t bank_index,
    history_reader_state& reader) noexcept {
    auto& bank = *state.banks[bank_index].bank;
    reader.bank_index = bank_index;
    reader.bank_attached = true;
    ++bank.reader_references;
    ++bank.active_operations;
}

bool allocate_reader_locked(history_store_state& state,
    connection_target target,
    std::size_t& reader_index,
    history_reader_handle& handle) noexcept {
    const auto first_reserved =
        state.readers.begin() + static_cast<std::ptrdiff_t>(public_history_reader_capacity);
    const auto free_reader = std::ranges::find_if(state.readers.begin(),
        first_reserved,
        [](const history_reader_state& reader) { return !reader.used; });
    if (free_reader == first_reserved) {
        return false;
    }

    const auto index = static_cast<std::size_t>(free_reader - state.readers.begin());
    return allocate_reader_at_locked(state, index, target, reader_index, handle);
}

bool allocate_xinput_reader_locked(history_store_state& state,
    std::uint32_t position,
    connection_target target,
    std::size_t& reader_index,
    history_reader_handle& handle) noexcept {
    if (position >= reserved_xinput_history_reader_capacity) {
        return false;
    }
    const auto index = public_history_reader_capacity + static_cast<std::size_t>(position);
    return allocate_reader_at_locked(state, index, target, reader_index, handle);
}

} // namespace inputnext::history_detail
