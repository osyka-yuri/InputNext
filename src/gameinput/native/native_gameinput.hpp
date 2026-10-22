#pragma once

#include "gameinput/reading_parts.hpp"
#include "native_rumble_endpoint.hpp"
#include <inputnext/device_metadata.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>

namespace inputnext::gameinput_detail {

class native_history_cursor;

struct native_device_handle final {
    void* value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != nullptr; }

    friend constexpr bool operator==(const native_device_handle&,
        const native_device_handle&) noexcept = default;
};

struct native_callback_token final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(const native_callback_token&,
        const native_callback_token&) noexcept = default;
};

enum class native_device_event : std::uint8_t {
    connected,
    disconnected,
    haptic_info_updated,
};

using device_delivery = void (*)(void* owner,
    native_device_handle device,
    native_device_event event) noexcept;
using reading_delivery = void (*)(void* owner, const reading_parts& parts) noexcept;

// Runtime-owned context addresses remain stable until unregister succeeds. Delivery functions run
// inline on the GameInput callback thread and must not allocate, log, query OS metadata, or perform
// lifecycle operations.
struct native_device_callback_context final {
    void* owner{};
    device_delivery deliver{};
};

struct native_reading_callback_context final {
    void* owner{};
    reading_delivery deliver{};
};

enum class native_reading_lane : std::uint8_t {
    gamepad,
    system_button,
};

enum class native_failure_cause : std::uint8_t {
    load_failed,
    api_unavailable,
    initialization_failed,
    registration_failed,
    no_current_reading,
    device_disconnected,
    resource_exhausted,
    clock_discontinuity,
    native_failure,
};

struct native_failure final {
    native_failure_cause cause{native_failure_cause::native_failure};
    std::uint32_t native_code{};
    // GameInput normally clears the token on failed registration. Preserve a nonzero partial
    // token so the coordinator can drain it before the context is reused.
    native_callback_token partial_token{};
};

template <typename T>
using native_result = std::expected<T, native_failure>;

class native_gameinput {
public:
    virtual ~native_gameinput() = default;

    native_gameinput(const native_gameinput&) = delete;
    native_gameinput& operator=(const native_gameinput&) = delete;
    native_gameinput(native_gameinput&&) = delete;
    native_gameinput& operator=(native_gameinput&&) = delete;

    [[nodiscard]] virtual native_result<void> initialize(
        const std::filesystem::path& runtime_library) = 0;
    [[nodiscard]] virtual native_result<native_callback_token> register_device_watch(
        native_device_callback_context& context) = 0;
    [[nodiscard]] virtual native_result<native_callback_token> register_device_rescan(
        native_device_callback_context& context) = 0;
    [[nodiscard]] virtual native_result<native_callback_token> register_reading_callback(
        native_device_handle device,
        native_reading_callback_context& context) = 0;
    [[nodiscard]] virtual bool unregister_callback(native_callback_token token) noexcept = 0;
    [[nodiscard]] virtual native_result<reading_parts> current(native_device_handle device,
        native_reading_lane lane) = 0;
    [[nodiscard]] virtual native_result<std::unique_ptr<native_history_cursor>>
    create_history_cursor(native_device_handle device) = 0;
    [[nodiscard]] virtual native_result<std::uint64_t> performance_frequency() noexcept = 0;
    [[nodiscard]] virtual native_result<native_device_facts> facts(native_device_handle device) = 0;
    [[nodiscard]] virtual bool connected(native_device_handle device) noexcept = 0;
    [[nodiscard]] virtual native_result<std::unique_ptr<native_rumble_endpoint>>
    create_rumble_endpoint(native_device_handle device) = 0;
    virtual void retain(native_device_handle device) noexcept = 0;
    virtual void release(native_device_handle device) noexcept = 0;

protected:
    native_gameinput() = default;
};

[[nodiscard]] std::unique_ptr<native_gameinput> make_native_gameinput();

} // namespace inputnext::gameinput_detail
