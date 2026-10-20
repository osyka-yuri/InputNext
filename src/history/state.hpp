#pragma once

#include "cursor.hpp"
#include "ring.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::history_detail {

inline constexpr std::size_t public_history_reader_capacity = 28U;
inline constexpr std::size_t reserved_xinput_history_reader_capacity = 4U;
inline constexpr std::size_t history_reader_capacity =
    public_history_reader_capacity + reserved_xinput_history_reader_capacity;
inline constexpr std::size_t resident_history_bank_capacity = 32U;
inline constexpr std::size_t history_work_per_read = 64U;
inline constexpr std::size_t invalid_bank_index = resident_history_bank_capacity;

static_assert(history_reader_capacity == resident_history_bank_capacity);

struct identity_seed final {
    std::uint64_t next_reader{1U};
    std::uint64_t next_stream{1U};
};

enum class bank_slot_state : std::uint8_t { vacant, opening, active, retiring, destroying };

struct history_bank final {
    explicit history_bank(connection_target exact_target,
        std::unique_ptr<history_cursor> source_cursor) noexcept;

    history_bank(const history_bank&) = delete;
    history_bank& operator=(const history_bank&) = delete;
    history_bank(history_bank&&) = delete;
    history_bank& operator=(history_bank&&) = delete;
    ~history_bank() = default;

    connection_target target{};
    std::unique_ptr<history_cursor> cursor;
    history_ring ring;
    std::mutex operation_gate;
    std::uint64_t reader_references{};
    std::uint64_t active_operations{};
    std::uint64_t qpc_frequency{};
    std::uint64_t checkpoint_position{};
    std::uint64_t checkpoint_qpc{};
    std::uint64_t last_qpc{};
    std::uint64_t recovery_lost_count{};
    history_source_kind source{history_source_kind::callback_capture};
    history_failure deferred_failure{};
    history_loss recovery_loss{history_loss::unknown};
    bool started{};
    bool recovery_pending{};
    bool terminal{};
};

struct history_bank_slot final {
    std::unique_ptr<history_bank> bank;
    // Preserve the reservation identity until a rejected provider cursor is destroyed.
    history_reader_handle opening_owner{};
    bank_slot_state state{bank_slot_state::vacant};
};

struct history_reader_state final {
    history_reader_handle handle{};
    std::size_t bank_index{};
    std::uint64_t stream_id{};
    std::uint64_t next_position{};
    std::uint64_t next_output_sequence{1U};
    ring_loss_mark loss_mark{};
    history_sample selected_baseline{};
    history_source_kind selected_source{history_source_kind::callback_capture};
    history_failure terminal_failure{.code = history_error::closed};
    bool used{};
    bool opening{};
    bool cancelled{};
    bool bank_attached{};
    bool operation_busy{};
    bool closing{};
    bool terminal{};
    bool selected_baseline_ready{};
};

struct history_store_state final {
    explicit history_store_state(history_provider& source, identity_seed seed) noexcept;

    history_provider& provider;
    mutable std::mutex gate;
    std::condition_variable operations_drained;
    std::array<history_bank_slot, resident_history_bank_capacity> banks{};
    std::array<history_reader_state, history_reader_capacity> readers{};
    std::uint64_t next_reader_id{};
    std::uint64_t next_stream_id{};
    std::size_t active_open_calls{};
    std::size_t active_read_calls{};
    std::size_t active_close_calls{};
    std::size_t active_drain_calls{};
    void (*before_read_commit_test_hook)(void*) noexcept {};
    void* before_read_commit_test_context{};
    bool closed{};
};

[[nodiscard]] std::optional<std::size_t> find_reader_locked(const history_store_state& state,
    history_reader_handle handle) noexcept;
[[nodiscard]] std::optional<std::size_t> find_active_bank_locked(const history_store_state& state,
    connection_target target) noexcept;
[[nodiscard]] std::optional<std::size_t> find_opening_bank_locked(const history_store_state& state,
    connection_target target) noexcept;
[[nodiscard]] std::optional<std::size_t> find_vacant_bank_locked(
    const history_store_state& state) noexcept;
void release_reader_bank_reference_locked(history_store_state& state,
    history_reader_state& reader) noexcept;
void clear_reader(history_reader_state& reader) noexcept;
void retire_target_locked(history_store_state& state, connection_target target) noexcept;
void retire_target(history_store_state& state, connection_target target) noexcept;
void reserve_bank_operation_locked(history_store_state& state,
    std::size_t bank_index,
    history_reader_state& reader) noexcept;
[[nodiscard]] bool allocate_reader_locked(history_store_state& state,
    connection_target target,
    std::size_t& reader_index,
    history_reader_handle& handle) noexcept;
[[nodiscard]] bool allocate_xinput_reader_locked(history_store_state& state,
    std::uint32_t position,
    connection_target target,
    std::size_t& reader_index,
    history_reader_handle& handle) noexcept;

} // namespace inputnext::history_detail
