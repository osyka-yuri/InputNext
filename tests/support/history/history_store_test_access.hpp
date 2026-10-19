#pragma once

#include "ring.hpp"
#include "state.hpp"
#include "store.hpp"
#include <inputnext/history.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace inputnext::history_detail {

struct history_ring_test_access final {
    static void set_empty_position(history_ring& ring,
        std::uint64_t position,
        ring_loss_mark mark = {}) noexcept {
        ring.oldest_position_ = position;
        ring.next_position_ = position;
        ring.tail_loss_mark_ = mark;
        ring.latest_unknown_cut_ = 0U;
    }
};

struct history_store_test_access final {
    using before_read_commit_hook = void (*)(void*) noexcept;

    static void set_before_read_commit_test_hook(history_store& store,
        before_read_commit_hook hook,
        void* context) noexcept {
        std::scoped_lock lock{store.state_.gate};
        store.state_.before_read_commit_test_hook = hook;
        store.state_.before_read_commit_test_context = context;
    }

    static void set_next_output_sequence(history_store& store,
        history_reader_handle handle,
        std::uint64_t sequence) noexcept {
        std::scoped_lock lock{store.state_.gate};
        for (auto& reader : store.state_.readers) {
            if (reader.used && reader.handle == handle) {
                reader.next_output_sequence = sequence;
                return;
            }
        }
    }

    static void set_ring_tail(history_store& store,
        history_reader_handle handle,
        std::uint64_t position) noexcept {
        std::size_t reader_index{};
        auto* bank = retain_reader_bank(store, handle, reader_index);
        if (bank == nullptr) {
            return;
        }

        const auto mark = set_ring_position(*bank, position);
        release_reader_bank(store, reader_index, handle, *bank, position, mark);
    }

private:
    [[nodiscard]] static history_bank* retain_reader_bank(history_store& store,
        history_reader_handle handle,
        std::size_t& reader_index) noexcept {
        std::scoped_lock lock{store.state_.gate};
        for (std::size_t index = 0U; index < store.state_.readers.size(); ++index) {
            const auto& reader = store.state_.readers[index];
            if (!reader.used || reader.handle != handle || !reader.bank_attached) {
                continue;
            }

            reader_index = index;
            auto* bank = store.state_.banks[reader.bank_index].bank.get();
            if (bank == nullptr ||
                bank->active_operations == std::numeric_limits<std::uint64_t>::max()) {
                return nullptr;
            }
            ++bank->active_operations;
            return bank;
        }
        return nullptr;
    }

    [[nodiscard]] static ring_loss_mark set_ring_position(history_bank& bank,
        std::uint64_t position) noexcept {
        std::scoped_lock lock{bank.operation_gate};
        history_ring_test_access::set_empty_position(bank.ring, position);
        return bank.ring.tail_loss_mark();
    }

    static void release_reader_bank(history_store& store,
        std::size_t reader_index,
        history_reader_handle handle,
        history_bank& bank,
        std::uint64_t position,
        ring_loss_mark mark) noexcept {
        std::scoped_lock lock{store.state_.gate};
        auto& reader = store.state_.readers[reader_index];
        if (reader.used && reader.handle == handle) {
            reader.next_position = position;
            reader.loss_mark = mark;
        }
        --bank.active_operations;
        store.state_.operations_drained.notify_all();
    }
};

} // namespace inputnext::history_detail
