#include "store.hpp"

#include "cursor.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace inputnext::history_detail {
namespace {

[[nodiscard]] history_failure failure(history_error code) noexcept {
    return history_failure{.code = code};
}

} // namespace

history_bank::history_bank(connection_target exact_target,
    std::unique_ptr<history_cursor> source_cursor) noexcept
    : target(exact_target),
      cursor(std::move(source_cursor)) {
}

history_store_state::history_store_state(history_provider& source, identity_seed seed) noexcept
    : provider(source),
      next_reader_id(seed.next_reader),
      next_stream_id(seed.next_stream) {
}

history_store::history_store(history_provider& provider, identity_seed seed)
    : state_(provider, seed) {
}

history_store::~history_store() noexcept {
    close_all();
    {
        std::unique_lock lock{state_.gate};
        state_.operations_drained.wait(lock, [this] {
            return state_.active_open_calls == 0U && state_.active_read_calls == 0U &&
                   state_.active_close_calls == 0U && state_.active_drain_calls == 0U &&
                   std::ranges::all_of(state_.banks, [](const history_bank_slot& slot) {
                       return slot.bank == nullptr || slot.bank->active_operations == 0U;
                   });
        });
    }

    while (resident_bank_count() != 0U) {
        if (drain_retired() == 0U) {
            std::unique_lock lock{state_.gate};
            state_.operations_drained.wait(lock, [this] {
                return std::ranges::all_of(state_.banks, [](const history_bank_slot& slot) {
                    return slot.state != bank_slot_state::destroying;
                });
            });
        }
    }
}

history_result<void> history_store::close(history_reader_handle reader) {
    if (!reader.target.valid() || reader.value == 0U) {
        return std::unexpected(failure(history_error::invalid_argument));
    }

    std::scoped_lock lock{state_.gate};
    if (state_.active_close_calls == std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(failure(history_error::capacity_exceeded));
    }
    ++state_.active_close_calls;

    const auto reader_index = find_reader_locked(state_, reader);
    if (!reader_index.has_value()) {
        --state_.active_close_calls;
        state_.operations_drained.notify_all();
        return std::unexpected(
            failure(state_.closed ? history_error::closed : history_error::invalid_reader));
    }

    auto& reader_state = state_.readers[*reader_index];
    if (!reader_state.closing) {
        reader_state.closing = true;
        reader_state.cancelled = reader_state.opening;
        release_reader_bank_reference_locked(state_, reader_state);
        if (!reader_state.operation_busy && !reader_state.opening) {
            clear_reader(reader_state);
        }
    }

    --state_.active_close_calls;
    state_.operations_drained.notify_all();
    return {};
}

void history_store::retire(connection_target target) noexcept {
    if (target.valid()) {
        retire_target(state_, target);
    }
}

void history_store::close_all() noexcept {
    std::scoped_lock lock{state_.gate};
    state_.closed = true;
    for (auto& slot : state_.banks) {
        if (slot.state == bank_slot_state::active) {
            slot.state = bank_slot_state::retiring;
        }
    }

    for (auto& reader : state_.readers) {
        if (!reader.used) {
            continue;
        }
        reader.terminal = true;
        reader.closing = true;
        reader.cancelled = reader.opening;
        reader.terminal_failure = failure(history_error::closed);
        release_reader_bank_reference_locked(state_, reader);
        if (!reader.operation_busy && !reader.opening) {
            clear_reader(reader);
        }
    }
    state_.operations_drained.notify_all();
}

std::size_t history_store::drain_retired() noexcept {
    std::array<std::unique_ptr<history_bank>, resident_history_bank_capacity> detached{};
    std::array<std::size_t, resident_history_bank_capacity> detached_indices{};
    std::size_t detached_count{};
    {
        std::scoped_lock lock{state_.gate};
        if (state_.active_drain_calls == std::numeric_limits<std::size_t>::max()) {
            return 0U;
        }
        ++state_.active_drain_calls;
        for (std::size_t index = 0U; index < state_.banks.size(); ++index) {
            auto& slot = state_.banks[index];
            if (slot.state != bank_slot_state::retiring || slot.bank == nullptr ||
                slot.bank->reader_references != 0U || slot.bank->active_operations != 0U) {
                continue;
            }
            slot.state = bank_slot_state::destroying;
            detached_indices[detached_count] = index;
            detached[detached_count] = std::move(slot.bank);
            ++detached_count;
        }
    }

    for (std::size_t index = 0U; index < detached_count; ++index) {
        detached[index].reset();
    }

    if (detached_count != 0U) {
        std::scoped_lock lock{state_.gate};
        for (std::size_t index = 0U; index < detached_count; ++index) {
            auto& slot = state_.banks[detached_indices[index]];
            if (slot.state == bank_slot_state::destroying && slot.bank == nullptr) {
                slot.state = bank_slot_state::vacant;
            }
        }
    }

    {
        std::scoped_lock lock{state_.gate};
        --state_.active_drain_calls;
        state_.operations_drained.notify_all();
    }

    return detached_count;
}

std::size_t history_store::resident_bank_count() const noexcept {
    std::scoped_lock lock{state_.gate};
    return static_cast<std::size_t>(std::ranges::count_if(state_.banks,
        [](const history_bank_slot& slot) { return slot.state != bank_slot_state::vacant; }));
}

} // namespace inputnext::history_detail
