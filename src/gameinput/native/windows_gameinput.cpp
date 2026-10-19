#include "native_gameinput.hpp"

#include "gameinput/reading_parts.hpp"
#include "gameinput_value_mapping.hpp"
#include "windows_history_cursor.hpp"
#include <inputnext/device_metadata.hpp>

#include <GameInput.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <ranges>

#include <Windows.h>
#include <profileapi.h>

#include <algorithm>
#include <bit>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

constexpr auto gamepad_and_system_button = std::bit_cast<gameinput::GameInputKind>(
    static_cast<std::uint32_t>(gameinput::GameInputKindGamepad) |
    static_cast<std::uint32_t>(gameinput::GameInputKindSystemButton));

using initialize_gameinput = HRESULT(WINAPI*)(REFIID interface_id, void** instance);

[[nodiscard]] std::uint32_t native_code(HRESULT result) noexcept {
    return static_cast<std::uint32_t>(result);
}

[[nodiscard]] HRESULT clock_error() noexcept {
    const auto error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
}

[[nodiscard]] native_failure failure(native_failure_cause cause, HRESULT result = E_FAIL) noexcept {
    return native_failure{.cause = cause, .native_code = native_code(result)};
}

[[nodiscard]] std::uint64_t observation_qpc() noexcept {
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value) == FALSE || value.QuadPart <= 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] std::optional<native_device_event> map_event(
    gameinput::GameInputDeviceEvent event) noexcept {
    switch (event) {
    case gameinput::GameInputDeviceEventConnected:
        return native_device_event::connected;
    case gameinput::GameInputDeviceEventDisconnected:
        return native_device_event::disconnected;
    case gameinput::GameInputDeviceEventHapticInfoUpdated:
        return native_device_event::haptic_info_updated;
    default:
        return std::nullopt;
    }
}

class gameinput_reading final {
public:
    explicit gameinput_reading(gameinput::IGameInputReading* reading) noexcept
        : reading_(reading) {}

    ~gameinput_reading() {
        if (reading_ != nullptr) {
            reading_->Release();
        }
    }

    gameinput_reading(const gameinput_reading&) = delete;
    gameinput_reading& operator=(const gameinput_reading&) = delete;
    gameinput_reading(gameinput_reading&&) = delete;
    gameinput_reading& operator=(gameinput_reading&&) = delete;

    [[nodiscard]] gameinput::IGameInputReading* get() const noexcept { return reading_; }

private:
    gameinput::IGameInputReading* reading_{};
};

class windows_gameinput final : public native_gameinput {
public:
    windows_gameinput() = default;
    windows_gameinput(const windows_gameinput&) = delete;
    windows_gameinput& operator=(const windows_gameinput&) = delete;
    windows_gameinput(windows_gameinput&&) = delete;
    windows_gameinput& operator=(windows_gameinput&&) = delete;

    ~windows_gameinput() override {
        if (game_input_ != nullptr) {
            game_input_->Release();
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    [[nodiscard]] native_result<void> initialize(
        const std::filesystem::path& runtime_library) override {
        if (!runtime_library.is_absolute() || runtime_library.empty()) {
            return std::unexpected(failure(native_failure_cause::load_failed, E_INVALIDARG));
        }

        module_ = LoadLibraryExW(runtime_library.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module_ == nullptr) {
            return std::unexpected(
                failure(native_failure_cause::load_failed, HRESULT_FROM_WIN32(GetLastError())));
        }

        const auto initialize =
            reinterpret_cast<initialize_gameinput>(GetProcAddress(module_, "GameInputInitialize"));
        if (initialize == nullptr) {
            auto error = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_PROC_NOT_FOUND;
            }
            return std::unexpected(
                failure(native_failure_cause::api_unavailable, HRESULT_FROM_WIN32(error)));
        }

        const auto result =
            initialize(gameinput::IID_IGameInput, reinterpret_cast<void**>(&game_input_));
        if (FAILED(result) || game_input_ == nullptr) {
            return std::unexpected(failure(native_failure_cause::initialization_failed, result));
        }

        game_input_->EnableBackgroundInput(true);
        return {};
    }

    [[nodiscard]] native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) override {
        return register_device_callback(context);
    }

    [[nodiscard]] native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) override {
        return register_device_callback(context);
    }

    [[nodiscard]] native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) override {
        if (game_input_ == nullptr || !device.valid()) {
            return std::unexpected(
                failure(native_failure_cause::registration_failed, E_INVALIDARG));
        }

        gameinput::GameInputCallbackToken token{};
        const auto result = game_input_->RegisterReadingCallback(as_device(device),
            gamepad_and_system_button,
            &context,
            &on_reading,
            &token);
        if (FAILED(result)) {
            auto native_error = failure(native_failure_cause::registration_failed, result);
            native_error.partial_token = native_callback_token{token};
            return std::unexpected(native_error);
        }
        return native_callback_token{token};
    }

    [[nodiscard]] bool unregister_callback(native_callback_token token) noexcept override {
        return game_input_ != nullptr && token.valid() &&
               game_input_->UnregisterCallback(token.value);
    }

    [[nodiscard]] native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) override {
        if (game_input_ == nullptr || !device.valid()) {
            return std::unexpected(failure(native_failure_cause::no_current_reading, E_INVALIDARG));
        }

        gameinput::IGameInputReading* raw_reading{};
        const auto kind = lane == native_reading_lane::gamepad
                              ? gameinput::GameInputKindGamepad
                              : gameinput::GameInputKindSystemButton;
        const auto result = game_input_->GetCurrentReading(kind, as_device(device), &raw_reading);
        if (FAILED(result) || raw_reading == nullptr) {
            if (raw_reading != nullptr) {
                raw_reading->Release();
            }
            const auto cause = result == gameinput::GAMEINPUT_E_DEVICE_DISCONNECTED
                                   ? native_failure_cause::device_disconnected
                                   : native_failure_cause::no_current_reading;
            return std::unexpected(failure(cause, result));
        }

        const gameinput_reading reading{raw_reading};
        return extract_parts(reading.get(), lane);
    }

    [[nodiscard]] native_result<std::unique_ptr<native_history_cursor>> create_history_cursor(
        native_device_handle device) override {
        if (game_input_ == nullptr || !device.valid()) {
            return std::unexpected(failure(native_failure_cause::no_current_reading, E_INVALIDARG));
        }
        try {
            return create_windows_history_cursor(game_input_, device);
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                failure(native_failure_cause::resource_exhausted, E_OUTOFMEMORY));
        }
    }

    [[nodiscard]] native_result<std::uint64_t> performance_frequency() noexcept override {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) == FALSE || frequency.QuadPart <= 0) {
            return std::unexpected(
                failure(native_failure_cause::clock_discontinuity, clock_error()));
        }
        return static_cast<std::uint64_t>(frequency.QuadPart);
    }

    [[nodiscard]] native_result<native_device_facts> facts(native_device_handle device) override {
        if (!device.valid()) {
            return std::unexpected(failure(native_failure_cause::native_failure, E_INVALIDARG));
        }

        auto* native_device = as_device(device);
        native_device_facts result{};
        result.vendor_id = available(native_device->GetVendorId());
        result.product_id = available(native_device->GetProductId());
        result.revision = available(native_device->GetRevisionNumber());

        APP_LOCAL_DEVICE_ID device_id{};
        APP_LOCAL_DEVICE_ID root_id{};
        native_device->GetDeviceId(&device_id);
        native_device->GetDeviceRootId(&root_id);
        result.gameinput_device_id = available(copy_device_id(device_id));
        result.gameinput_root_id = available(copy_device_id(root_id));

        GUID container{};
        native_device->GetContainerId(&container);
        const auto copied_container = copy_guid(container);
        result.container_id =
            copied_container.is_zero()
                ? metadata_field<native_guid>{.availability = metadata_availability::not_present}
                : available(copied_container);

        const auto supported_input = native_device->GetSupportedInput();
        const auto rumble = native_device->GetSupportedRumbleMotors();
        result.gameinput_capabilities = available(native_gameinput_capabilities{
            .gamepad = (supported_input & gameinput::GameInputKindGamepad) != 0,
            .low_frequency_rumble = (rumble & gameinput::GameInputRumbleLowFrequency) != 0,
            .high_frequency_rumble = (rumble & gameinput::GameInputRumbleHighFrequency) != 0,
            .left_trigger_rumble = (rumble & gameinput::GameInputRumbleLeftTrigger) != 0,
            .right_trigger_rumble = (rumble & gameinput::GameInputRumbleRightTrigger) != 0,
        });

        result.pnp_path_utf8 = read_pnp_path(native_device);
        auto haptic_endpoint = read_haptic_endpoint(native_device);
        if (!haptic_endpoint.has_value()) {
            return std::unexpected(haptic_endpoint.error());
        }
        result.haptic_endpoint_id = std::move(*haptic_endpoint);
        return result;
    }

    [[nodiscard]] bool connected(native_device_handle device) noexcept override {
        return device.valid() && as_device(device)->IsConnected();
    }

    void retain(native_device_handle device) noexcept override {
        if (device.valid()) {
            as_device(device)->AddRef();
        }
    }

    void release(native_device_handle device) noexcept override {
        if (device.valid()) {
            as_device(device)->Release();
        }
    }

private:
    [[nodiscard]] static gameinput::IGameInputDevice* as_device(
        native_device_handle device) noexcept {
        return static_cast<gameinput::IGameInputDevice*>(device.value);
    }

    [[nodiscard]] native_result<native_callback_token> register_device_callback(
        native_device_callback_context& context) {
        if (game_input_ == nullptr) {
            return std::unexpected(
                failure(native_failure_cause::registration_failed, E_UNEXPECTED));
        }

        gameinput::GameInputCallbackToken token{};
        const auto result = game_input_->RegisterDeviceCallback(nullptr,
            gameinput::GameInputKindGamepad,
            &context,
            &on_device_event,
            &token);
        if (FAILED(result)) {
            auto native_error = failure(native_failure_cause::registration_failed, result);
            native_error.partial_token = native_callback_token{token};
            return std::unexpected(native_error);
        }
        return native_callback_token{token};
    }

    [[nodiscard]] static reading_parts extract_parts(gameinput::IGameInputReading* reading,
        native_reading_lane lane) noexcept {
        reading_parts result{};
        result.source_timestamp_us = reading->GetTimestamp();
        result.observation_qpc = observation_qpc();

        if (lane == native_reading_lane::gamepad) {
            gameinput::GameInputGamepadState state{};
            if (reading->GetGamepadState(&state)) {
                result.pad = map_gamepad_state(state);
            }
        } else {
            gameinput::GameInputSystemButtons buttons{};
            if (reading->GetSystemButtonState(&buttons)) {
                result.guide = (buttons & gameinput::GameInputSystemButtonGuide) != 0;
            }
        }
        return result;
    }

    static void CALLBACK on_device_event(gameinput::GameInputCallbackToken callback_token,
        void* context,
        gameinput::GameInputDeviceEvent event,
        gameinput::IGameInputDevice* device) noexcept {
        (void)callback_token;
        auto const* callback_context = static_cast<native_device_callback_context*>(context);
        const auto mapped_event = map_event(event);
        if (callback_context != nullptr && callback_context->deliver != nullptr &&
            device != nullptr && mapped_event.has_value()) {
            callback_context->deliver(callback_context->owner,
                native_device_handle{device},
                *mapped_event);
        }
    }

    static void CALLBACK on_reading(gameinput::GameInputCallbackToken callback_token,
        void* context,
        gameinput::IGameInputReading* reading) noexcept {
        (void)callback_token;
        auto const* callback_context = static_cast<native_reading_callback_context*>(context);
        if (callback_context != nullptr && callback_context->deliver != nullptr &&
            reading != nullptr) {
            const auto parts = extract_callback_parts(reading);
            callback_context->deliver(callback_context->owner, parts);
        }
    }

    [[nodiscard]] static reading_parts extract_callback_parts(
        gameinput::IGameInputReading* reading) noexcept {
        reading_parts result{};
        result.source_timestamp_us = reading->GetTimestamp();
        result.observation_qpc = observation_qpc();

        gameinput::GameInputGamepadState pad{};
        if (reading->GetGamepadState(&pad)) {
            result.pad = map_gamepad_state(pad);
        }
        gameinput::GameInputSystemButtons buttons{};
        if (reading->GetSystemButtonState(&buttons)) {
            result.guide = (buttons & gameinput::GameInputSystemButtonGuide) != 0;
        }
        return result;
    }

    template <typename T>
    [[nodiscard]] static metadata_field<T> available(T value) {
        return metadata_field<T>{.availability = metadata_availability::available,
            .value = std::move(value)};
    }

    template <typename T>
    [[nodiscard]] static metadata_field<T> unavailable() {
        return metadata_field<T>{.availability = metadata_availability::unavailable};
    }

    [[nodiscard]] static metadata_field<std::string> read_pnp_path(
        gameinput::IGameInputDevice* device) {
        const auto required = device->GetPnpPath(0U, nullptr);
        if (required == 0U) {
            return unavailable<std::string>();
        }
        if (required > max_pnp_path_utf8_bytes + 1U) {
            return metadata_field<std::string>{
                .availability = metadata_availability::capacity_exceeded};
        }

        std::string path(required, '\0');
        const auto returned = device->GetPnpPath(required, path.data());
        if (returned == 0U || returned > required) {
            return unavailable<std::string>();
        }
        const auto view = std::string_view{path.data(), returned};
        const auto length = view.find('\0');
        if (length == std::string_view::npos) {
            return metadata_field<std::string>{
                .availability = metadata_availability::capacity_exceeded};
        }
        if (length > max_pnp_path_utf8_bytes) {
            return metadata_field<std::string>{
                .availability = metadata_availability::capacity_exceeded};
        }
        path.resize(length);
        if (path.empty()) {
            return metadata_field<std::string>{.availability = metadata_availability::not_present};
        }
        return available(std::move(path));
    }

    [[nodiscard]] static native_result<metadata_field<std::u16string>> read_haptic_endpoint(
        gameinput::IGameInputDevice* device) {
        static_assert(sizeof(wchar_t) == sizeof(char16_t));
        gameinput::GameInputHapticInfo info{};
        const auto result = device->GetHapticInfo(&info);
        const auto result_class = classify_haptic_info_result(result);
        if (result_class == haptic_info_result::not_present) {
            return metadata_field<std::u16string>{.availability =
                                                      metadata_availability::not_present,
                .native_code = native_code(result)};
        }
        if (result_class == haptic_info_result::failure) {
            return std::unexpected(failure(native_failure_cause::native_failure, result));
        }

        auto const* const end = std::ranges::find(info.audioEndpointId, L'\0');
        if (end == std::ranges::end(info.audioEndpointId)) {
            return metadata_field<std::u16string>{
                .availability = metadata_availability::capacity_exceeded};
        }
        const auto length =
            static_cast<std::size_t>(end - std::ranges::begin(info.audioEndpointId));
        if (length > haptic_endpoint_content_code_units) {
            return metadata_field<std::u16string>{
                .availability = metadata_availability::capacity_exceeded};
        }
        if (length == 0U) {
            return metadata_field<std::u16string>{
                .availability = metadata_availability::not_present};
        }

        std::u16string endpoint;
        endpoint.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            endpoint.push_back(static_cast<char16_t>(info.audioEndpointId[index]));
        }
        return available(std::move(endpoint));
    }

    HMODULE module_{};
    gameinput::IGameInput* game_input_{};
};

} // namespace

std::unique_ptr<native_gameinput> make_native_gameinput() {
    return std::make_unique<windows_gameinput>();
}

} // namespace inputnext::gameinput_detail
