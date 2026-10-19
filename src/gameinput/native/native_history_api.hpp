#pragma once

#include "native_gameinput.hpp"
#include <inputnext/gamepad_state.hpp>

#include <cstdint>

namespace inputnext::gameinput_detail {

enum class native_history_read_kind : std::uint8_t {
    reading,
    no_reading,
    reference_too_old,
    disconnected,
    failure,
};

struct native_history_read final {
    native_history_read_kind kind{native_history_read_kind::failure};
    void* reading{};
    std::uint32_t native_code{};
};

class native_history_api {
public:
    virtual ~native_history_api() = default;

    native_history_api(const native_history_api&) = delete;
    native_history_api& operator=(const native_history_api&) = delete;
    native_history_api(native_history_api&&) = delete;
    native_history_api& operator=(native_history_api&&) = delete;

    virtual void retain_session(native_device_handle device) noexcept = 0;
    virtual void release_session(native_device_handle device) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t current_timestamp() noexcept = 0;
    [[nodiscard]] virtual native_result<std::uint64_t> query_qpc() noexcept = 0;
    [[nodiscard]] virtual native_result<std::uint64_t> query_qpc_frequency() noexcept = 0;
    [[nodiscard]] virtual native_history_read current_pad(native_device_handle device) noexcept = 0;
    [[nodiscard]] virtual native_history_read next_pad(native_device_handle device,
        void* reference) noexcept = 0;
    [[nodiscard]] virtual std::uint64_t reading_timestamp(void* reading) noexcept = 0;
    [[nodiscard]] virtual native_result<gamepad_state> read_gamepad(void* reading) noexcept = 0;
    virtual void release_reading(void* reading) noexcept = 0;

protected:
    native_history_api() = default;
};

} // namespace inputnext::gameinput_detail
