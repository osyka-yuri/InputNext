#include "windows_history_cursor.hpp"

#include "gameinput_value_mapping.hpp"
#include "native_gameinput.hpp"
#include "native_history.hpp"
#include "native_history_api.hpp"
#include "native_history_cursor.hpp"
#include <inputnext/gamepad_state.hpp>

#include <expected>
#include <utility>

#include <Windows.h>

#include <GameInput.h>

#include <cstdint>
#include <memory>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

[[nodiscard]] std::uint32_t native_code(HRESULT result) noexcept {
    return static_cast<std::uint32_t>(result);
}

[[nodiscard]] HRESULT clock_error() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
}

[[nodiscard]] native_history_read failed_reading(native_history_read_kind kind,
    HRESULT result,
    gameinput::IGameInputReading* reading = nullptr) noexcept {
    if (reading != nullptr) {
        reading->Release();
    }
    return native_history_read{
        .kind = kind,
        .native_code = native_code(result),
    };
}

[[nodiscard]] native_history_read read_result(HRESULT result,
    gameinput::IGameInputReading* reading) noexcept {
    if (SUCCEEDED(result)) {
        if (reading == nullptr) {
            return failed_reading(native_history_read_kind::failure, E_UNEXPECTED);
        }
        return native_history_read{
            .kind = native_history_read_kind::reading,
            .reading = reading,
        };
    }

    return failed_reading(classify_history_failure(native_code(result)), result, reading);
}

class windows_history_api final : public native_history_api {
public:
    explicit windows_history_api(gameinput::IGameInput* game_input) noexcept
        : game_input_(game_input) {}

    void retain_session(native_device_handle device) noexcept override {
        game_input_->AddRef();
        as_device(device)->AddRef();
    }

    void release_session(native_device_handle device) noexcept override {
        as_device(device)->Release();
        game_input_->Release();
    }

    [[nodiscard]] std::uint64_t current_timestamp() noexcept override {
        return game_input_->GetCurrentTimestamp();
    }

    [[nodiscard]] native_result<std::uint64_t> query_qpc() noexcept override {
        LARGE_INTEGER counter{};
        if (QueryPerformanceCounter(&counter) == FALSE || counter.QuadPart <= 0) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::clock_discontinuity,
                .native_code = native_code(clock_error()),
            });
        }
        return static_cast<std::uint64_t>(counter.QuadPart);
    }

    [[nodiscard]] native_result<std::uint64_t> query_qpc_frequency() noexcept override {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::clock_discontinuity,
                .native_code = native_code(clock_error()),
            });
        }
        return static_cast<std::uint64_t>(frequency.QuadPart);
    }

    [[nodiscard]] native_history_read current_pad(native_device_handle device) noexcept override {
        if (!device.valid()) {
            return failed_reading(native_history_read_kind::failure, E_INVALIDARG);
        }
        gameinput::IGameInputReading* reading{};
        const auto result = game_input_->GetCurrentReading(gameinput::GameInputKindGamepad,
            as_device(device),
            &reading);
        return read_result(result, reading);
    }

    [[nodiscard]] native_history_read next_pad(native_device_handle device,
        void* reference) noexcept override {
        if (!device.valid() || reference == nullptr) {
            return failed_reading(native_history_read_kind::failure, E_INVALIDARG);
        }
        gameinput::IGameInputReading* reading{};
        const auto result =
            game_input_->GetNextReading(static_cast<gameinput::IGameInputReading*>(reference),
                gameinput::GameInputKindGamepad,
                as_device(device),
                &reading);
        return read_result(result, reading);
    }

    [[nodiscard]] std::uint64_t reading_timestamp(void* reading) noexcept override {
        return reading != nullptr
                   ? static_cast<gameinput::IGameInputReading*>(reading)->GetTimestamp()
                   : 0U;
    }

    [[nodiscard]] native_result<gamepad_state> read_gamepad(void* reading) noexcept override {
        if (reading == nullptr) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
                .native_code = native_code(E_INVALIDARG),
            });
        }

        gameinput::GameInputGamepadState state{};
        auto* native_reading = static_cast<gameinput::IGameInputReading*>(reading);
        if (!native_reading->GetGamepadState(&state)) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
                .native_code = native_code(E_FAIL),
            });
        }
        auto mapped = map_gamepad_state(state);
        if (!is_valid_gamepad_state(mapped)) {
            return std::unexpected(native_failure{
                .cause = native_failure_cause::native_failure,
                .native_code = native_code(E_INVALIDARG),
            });
        }
        return mapped;
    }

    void release_reading(void* reading) noexcept override {
        if (reading != nullptr) {
            static_cast<gameinput::IGameInputReading*>(reading)->Release();
        }
    }

private:
    [[nodiscard]] static gameinput::IGameInputDevice* as_device(
        native_device_handle device) noexcept {
        return static_cast<gameinput::IGameInputDevice*>(device.value);
    }

    gameinput::IGameInput* game_input_{};
};

} // namespace

native_history_read_kind classify_history_failure(std::uint32_t result_code) noexcept {
    if (result_code == native_code(gameinput::GAMEINPUT_E_READING_NOT_FOUND)) {
        return native_history_read_kind::no_reading;
    }
    if (result_code == native_code(gameinput::GAMEINPUT_E_REFERENCE_READING_TOO_OLD)) {
        return native_history_read_kind::reference_too_old;
    }
    if (result_code == native_code(gameinput::GAMEINPUT_E_DEVICE_DISCONNECTED)) {
        return native_history_read_kind::disconnected;
    }
    return native_history_read_kind::failure;
}

native_result<std::unique_ptr<native_history_cursor>>
create_windows_history_cursor(gameinput::IGameInput* game_input, native_device_handle device) {
    if (game_input == nullptr || !device.valid()) {
        return std::unexpected(native_failure{
            .cause = native_failure_cause::no_current_reading,
            .native_code = native_code(E_INVALIDARG),
        });
    }

    auto api = std::make_unique<windows_history_api>(game_input);
    auto cursor = std::make_unique<native_history_cursor_impl>(std::move(api), device);
    return std::unique_ptr<native_history_cursor>{std::move(cursor)};
}

} // namespace inputnext::gameinput_detail
