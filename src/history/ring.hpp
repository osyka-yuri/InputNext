#pragma once

#include <inputnext/history.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace inputnext::history_detail {

inline constexpr std::size_t history_bank_capacity = 256U;

enum class ring_entry_kind : std::uint8_t { reading, initial_baseline, recovery_baseline };

struct ring_loss_mark final {
    std::uint64_t known_prefix{};

    friend constexpr bool operator==(const ring_loss_mark&,
        const ring_loss_mark&) noexcept = default;
};

struct ring_entry final {
    std::uint64_t position{};
    history_sample sample{};
    history_source_kind source{history_source_kind::callback_capture};
    ring_entry_kind kind{ring_entry_kind::reading};
    history_loss loss{history_loss::none};
    std::uint64_t lost_count{};
    history_record_origin origin{history_record_origin::baseline};
    ring_loss_mark loss_mark{};
};

struct ring_overrun final {
    history_loss loss{history_loss::known};
    std::uint64_t lost_count{};
};

class history_ring final {
public:
    [[nodiscard]] bool append(ring_entry entry, std::uint64_t& position) noexcept;
    [[nodiscard]] const ring_entry* find(std::uint64_t position) const noexcept;
    [[nodiscard]] const ring_entry* latest() const noexcept;
    [[nodiscard]] ring_overrun overrun(std::uint64_t next_position,
        ring_loss_mark before_next) const noexcept;

    [[nodiscard]] ring_loss_mark tail_loss_mark() const noexcept { return tail_loss_mark_; }

    [[nodiscard]] std::uint64_t oldest_position() const noexcept { return oldest_position_; }

    [[nodiscard]] std::uint64_t next_position() const noexcept { return next_position_; }

    [[nodiscard]] bool empty() const noexcept { return oldest_position_ == next_position_; }

private:
    friend struct history_ring_test_access;

    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t position) noexcept {
        return static_cast<std::size_t>(position % history_bank_capacity);
    }

    std::array<ring_entry, history_bank_capacity> entries_{};
    std::uint64_t oldest_position_{1U};
    std::uint64_t next_position_{1U};
    ring_loss_mark tail_loss_mark_{};
    std::uint64_t latest_unknown_cut_{};
};

} // namespace inputnext::history_detail
