#pragma once

#include "input/touch_resolver.hpp"
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

struct native_history_frame final {
    gamepad_state pad{};
    input::touch_observation touch{};
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

    // Composite methods are opt-in and fail closed by default. Implementations must return a
    // complete gamepad/click observation from one exact reading; they must not join adjacent
    // readings or infer unchanged state from missing values.
    [[nodiscard]] virtual native_history_read current_composite(native_device_handle device,
        const native_input_profile& profile) noexcept {
        static_cast<void>(device);
        static_cast<void>(profile);
        return native_history_read{.kind = native_history_read_kind::failure};
    }

    [[nodiscard]] virtual native_history_read next_composite(native_device_handle device,
        void* reference,
        const native_input_profile& profile) noexcept {
        static_cast<void>(device);
        static_cast<void>(reference);
        static_cast<void>(profile);
        return native_history_read{.kind = native_history_read_kind::failure};
    }

    [[nodiscard]] virtual native_history_read previous_composite(native_device_handle device,
        void* reference,
        const native_input_profile& profile) noexcept {
        static_cast<void>(device);
        static_cast<void>(reference);
        static_cast<void>(profile);
        return native_history_read{.kind = native_history_read_kind::failure};
    }

    [[nodiscard]] virtual bool same_reading(void* first, void* second) noexcept {
        return first != nullptr && first == second;
    }

    [[nodiscard]] virtual native_result<native_history_frame> read_composite(void* reading,
        const native_input_profile& profile) noexcept {
        static_cast<void>(profile);
        auto pad = read_gamepad(reading);
        if (!pad.has_value()) {
            return std::unexpected(pad.error());
        }
        auto touch = input::touch_observation{};
        touch.click_status = input::observation_status::unavailable;
        touch.contacts_status = input::observation_status::not_applicable;
        return native_history_frame{.pad = *pad, .touch = touch};
    }

    [[nodiscard]] virtual std::uint64_t reading_timestamp(void* reading) noexcept = 0;
    [[nodiscard]] virtual native_result<gamepad_state> read_gamepad(void* reading) noexcept = 0;
    virtual void release_reading(void* reading) noexcept = 0;

protected:
    native_history_api() = default;
};

} // namespace inputnext::gameinput_detail
