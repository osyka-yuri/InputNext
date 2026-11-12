#pragma once

#include "input/program.hpp"
#include <inputnext/history.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace inputnext::gameinput_detail {

inline constexpr std::size_t history_capture_capacity = 256U;

enum class capture_record_kind : std::uint8_t {
    pad,
    guide,
    guide_cut,
    fallback_cut,
    touch_cut,
};

struct capture_record final {
    capture_record_kind kind{capture_record_kind::pad};
    input::touch_source touch{input::touch_source::none};
    std::uint64_t position{};
    history_sample sample{};
};

// Keep the additional source in the original control-header padding; retention has the same
// sample count and byte bound as the gamepad-only capture log.
static_assert(sizeof(capture_record) <= sizeof(history_sample) + (2U * sizeof(std::uint64_t)));

enum class capture_read_kind : std::uint8_t {
    record,
    end,
    overwritten,
    closed,
    exhausted,
};

struct capture_read final {
    capture_read_kind kind{capture_read_kind::end};
    capture_record record{};
    std::uint64_t oldest_position{1U};
    std::uint64_t tail_position{};
    std::uint64_t overwritten_control_position{};
};

struct capture_log_test_access;

class history_capture_log final {
public:
    history_capture_log() noexcept = default;
    ~history_capture_log() = default;

    history_capture_log(const history_capture_log&) = delete;
    history_capture_log& operator=(const history_capture_log&) = delete;
    history_capture_log(history_capture_log&&) = delete;
    history_capture_log& operator=(history_capture_log&&) = delete;

    [[nodiscard]] std::uint64_t next_position() const noexcept;
    [[nodiscard]] std::uint64_t oldest_position() const noexcept;
    [[nodiscard]] std::uint64_t tail_position() const noexcept;
    [[nodiscard]] std::uint64_t overwritten_control_position() const noexcept;
    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] bool closed() const noexcept;

    void append(capture_record_kind kind,
        const history_sample& sample,
        input::touch_source touch = input::touch_source::none) noexcept;
    void exhaust() noexcept;
    void close() noexcept;
    [[nodiscard]] capture_read read(std::uint64_t position) const noexcept;

private:
    friend struct capture_log_test_access;

    [[nodiscard]] static bool is_control(capture_record_kind kind) noexcept;

    std::array<capture_record, history_capture_capacity> records_{};
    std::uint64_t next_position_{1U};
    std::uint64_t tail_position_{};
    std::uint64_t overwritten_control_position_{};
    std::size_t size_{};
    bool exhausted_{};
    bool closed_{};
};

} // namespace inputnext::gameinput_detail
