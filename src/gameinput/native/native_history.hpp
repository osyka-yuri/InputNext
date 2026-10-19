#pragma once

#include "native_gameinput.hpp"

#include <inputnext/input_sample.hpp>

#include <cstdint>

namespace inputnext::gameinput_detail {

struct native_history_seed final {
    input_update baseline{};
    std::uint64_t qpc_frequency{};
};

enum class native_history_step_kind : std::uint8_t {
    reading,
    caught_up,
    history_lost,
    provider_failure,
    clock_discontinuity,
    disconnected,
    exhausted,
};

struct native_history_step final {
    native_history_step_kind kind{native_history_step_kind::provider_failure};
    input_update reading{};
    std::uint64_t checkpoint_qpc{};
    std::uint32_t native_code{};
};

class native_history_cursor {
public:
    virtual ~native_history_cursor() = default;

    native_history_cursor(const native_history_cursor&) = delete;
    native_history_cursor& operator=(const native_history_cursor&) = delete;
    native_history_cursor(native_history_cursor&&) = delete;
    native_history_cursor& operator=(native_history_cursor&&) = delete;

    [[nodiscard]] virtual native_result<native_history_seed> start() noexcept = 0;
    [[nodiscard]] virtual native_history_step next() noexcept = 0;

protected:
    native_history_cursor() = default;
};

} // namespace inputnext::gameinput_detail
