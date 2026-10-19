#pragma once

#include "device_identity.hpp"
#include "gamepad_state.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace inputnext {

using history_time = std::chrono::steady_clock::time_point;

inline constexpr std::size_t history_batch_capacity = 32U;

enum class history_error : std::uint8_t {
    invalid_argument,
    invalid_reader,
    closed,
    disconnected,
    busy,
    capacity_exceeded,
    identity_exhausted,
    sequence_exhausted,
    no_data,
    history_lost,
    provider_failure,
    clock_discontinuity,
};

struct history_failure final {
    history_error code{history_error::provider_failure};
    std::uint32_t native_code{};

    friend constexpr bool operator==(const history_failure&,
        const history_failure&) noexcept = default;
};

template <typename T>
using history_result = std::expected<T, history_failure>;

struct history_reader_handle final {
    // Valid only with the history store that created it; it does not extend runtime lifetime.
    connection_target target{};
    std::uint64_t value{};

    friend constexpr bool operator==(const history_reader_handle&,
        const history_reader_handle&) noexcept = default;
};

struct history_domain final {
    connection_target target{};
    std::uint64_t stream{};

    friend constexpr bool operator==(const history_domain&,
        const history_domain&) noexcept = default;
};

enum class history_loss : std::uint8_t { none, known, unknown };
enum class history_reset : std::uint8_t { none, initial_open, history_gap };
enum class history_source_kind : std::uint8_t { native_history, callback_capture };
enum class history_record_origin : std::uint8_t {
    baseline,
    native_pad,
    callback_pad,
    callback_guide
};

struct history_sample final {
    gamepad_state state{};
    std::uint64_t source_timestamp_us{};
    std::uint64_t effective_qpc{};
    bool guide_known{};

    friend constexpr bool operator==(const history_sample&,
        const history_sample&) noexcept = default;
};

struct history_record final {
    history_sample sample{};
    std::uint64_t sequence{};
    bool baseline{};
    history_record_origin origin{history_record_origin::baseline};

    friend constexpr bool operator==(const history_record&,
        const history_record&) noexcept = default;
};

struct history_batch final {
    // Each batch has one domain. Its counted records are a valid prefix; process them before
    // handling `failure`, which describes why traversal stopped. A reset baseline is returned alone.
    std::array<history_record, history_batch_capacity> records{};
    std::size_t count{};
    history_domain domain{};
    history_source_kind source{history_source_kind::callback_capture};
    history_reset reset{history_reset::none};
    history_loss loss{history_loss::none};
    std::uint64_t lost_count{};
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    std::uint64_t checkpoint_sequence{};
    std::uint64_t checkpoint_qpc{};
    bool caught_up{};
    std::optional<history_failure> failure;

    friend constexpr bool operator==(const history_batch&, const history_batch&) noexcept = default;
};

} // namespace inputnext
