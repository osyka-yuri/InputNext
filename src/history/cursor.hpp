#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/history.hpp>

#include <cstdint>
#include <memory>

namespace inputnext::history_detail {

struct cursor_seed final {
    history_sample baseline{};
    std::uint64_t qpc_frequency{};
    history_source_kind source{history_source_kind::callback_capture};
};

enum class cursor_step_kind : std::uint8_t {
    reading,
    progress,
    caught_up,
    history_lost,
    provider_failure,
    clock_discontinuity,
    disconnected,
    exhausted,
};

struct cursor_step final {
    cursor_step_kind kind{cursor_step_kind::provider_failure};
    history_sample reading{};
    std::uint64_t checkpoint_qpc{};
    history_loss loss{history_loss::none};
    std::uint64_t lost_count{};
    std::uint32_t native_code{};
    history_record_origin origin{history_record_origin::baseline};
};

class history_cursor {
public:
    virtual ~history_cursor() = default;

    history_cursor(const history_cursor&) = delete;
    history_cursor& operator=(const history_cursor&) = delete;
    history_cursor(history_cursor&&) = delete;
    history_cursor& operator=(history_cursor&&) = delete;

    [[nodiscard]] virtual history_result<cursor_seed> start(history_time now) noexcept = 0;
    [[nodiscard]] virtual cursor_step next(history_time now) noexcept = 0;

protected:
    history_cursor() = default;
};

class history_provider {
public:
    virtual ~history_provider() = default;

    history_provider(const history_provider&) = delete;
    history_provider& operator=(const history_provider&) = delete;
    history_provider(history_provider&&) = delete;
    history_provider& operator=(history_provider&&) = delete;

    [[nodiscard]] virtual history_result<std::unique_ptr<history_cursor>> open_cursor(
        connection_target target) = 0;
    [[nodiscard]] virtual bool is_live(connection_target target) noexcept = 0;

protected:
    history_provider() = default;
};

} // namespace inputnext::history_detail
