#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <cstdint>
#include <expected>

namespace inputnext::xinput {

class event_source {
public:
    virtual ~event_source() = default;

    event_source(const event_source&) = delete;
    event_source& operator=(const event_source&) = delete;
    event_source(event_source&&) = delete;
    event_source& operator=(event_source&&) = delete;

    event_source() = default;

    [[nodiscard]] virtual history_result<history_reader_handle>
    open_history(std::uint32_t position, connection_target target, history_time now) noexcept = 0;
    [[nodiscard]] virtual history_batch read_history(history_reader_handle reader,
        history_time now) noexcept = 0;
    [[nodiscard]] virtual history_result<void> close_history(
        history_reader_handle reader) noexcept = 0;
};

struct guide_wait_ticket final {
    std::uint32_t user_index{};
    std::uint64_t id{};

    friend bool operator==(const guide_wait_ticket&, const guide_wait_ticket&) = default;
};

enum class guide_wait_error : std::uint8_t {
    invalid_argument,
    invalid_ticket,
    not_supported,
    not_connected,
    ineligible,
    capacity_exceeded,
    identity_exhausted,
    busy,
    cancelled,
    history_reset,
    closed,
};

template <typename T>
using guide_wait_result = std::expected<T, guide_wait_error>;

} // namespace inputnext::xinput
