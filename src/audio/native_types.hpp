#pragma once

#include <inputnext/device_metadata.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace inputnext::audio_detail {

inline constexpr std::size_t native_render_channels = 4U;

enum class native_operation : std::uint8_t {
    none,
    create_wake_event,
    create_stop_event,
    create_buffer_event,
    create_native_port,
    close_buffer_event,
    wake_worker,
    request_stop,
    begin_worker_sta,
    create_device_enumerator,
    get_device,
    get_endpoint_id,
    get_endpoint_state,
    get_endpoint_flow,
    open_property_store,
    get_container_id,
    activate_audio_client,
    get_mix_format,
    is_format_supported,
    initialize,
    set_event_handle,
    get_render_service,
    get_buffer_size,
    start,
    stop,
    get_current_padding,
    get_buffer,
    release_buffer,
    wait,
    register_mmcss,
    revert_mmcss,
    finish_worker_sta,
};

struct native_error final {
    native_operation operation{native_operation::none};
    // HRESULT bit pattern for COM/audio calls; Win32 last-error value for handle operations.
    std::uint32_t native_code{};

    friend constexpr bool operator==(const native_error&, const native_error&) noexcept = default;
};

template <typename T>
using native_result = std::expected<T, native_error>;

enum class fixed_haptic_endpoint_id_error : std::uint8_t {
    empty,
    too_long,
    embedded_null,
};

// An owned, terminated copy of the exact metadata endpoint ID. Construction rejects bad input;
// it never normalizes or truncates an ID.
class fixed_haptic_endpoint_id final {
public:
    [[nodiscard]] static std::expected<fixed_haptic_endpoint_id, fixed_haptic_endpoint_id_error>
    create(const std::u16string_view value) noexcept {
        if (value.empty()) {
            return std::unexpected(fixed_haptic_endpoint_id_error::empty);
        }
        if (value.size() > haptic_endpoint_content_code_units) {
            return std::unexpected(fixed_haptic_endpoint_id_error::too_long);
        }
        if (std::ranges::find(value, u'\0') != value.end()) {
            return std::unexpected(fixed_haptic_endpoint_id_error::embedded_null);
        }

        fixed_haptic_endpoint_id result;
        std::ranges::copy(value, result.storage_.begin());
        result.length_ = static_cast<std::uint16_t>(value.size());
        return result;
    }

    [[nodiscard]] constexpr std::u16string_view view() const noexcept {
        return {storage_.data(), length_};
    }

    [[nodiscard]] constexpr const char16_t* terminated_data() const noexcept {
        return storage_.data();
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return length_; }

    friend constexpr bool operator==(const fixed_haptic_endpoint_id&,
        const fixed_haptic_endpoint_id&) noexcept = default;

private:
    std::array<char16_t, haptic_endpoint_buffer_code_units> storage_{};
    std::uint16_t length_{};
};

enum class native_endpoint_state : std::uint8_t {
    active,
    disabled,
    not_present,
    unplugged,
    unknown,
};

enum class native_endpoint_flow : std::uint8_t {
    render,
    capture,
    unknown,
};

struct native_endpoint_snapshot final {
    fixed_haptic_endpoint_id id;
    native_endpoint_state state{native_endpoint_state::unknown};
    native_endpoint_flow flow{native_endpoint_flow::unknown};
    native_guid container_id{};

    friend constexpr bool operator==(const native_endpoint_snapshot&,
        const native_endpoint_snapshot&) noexcept = default;
};

enum class native_sample_encoding : std::uint8_t {
    unknown,
    ieee_float,
    pcm_integer,
};

struct native_audio_format final {
    native_sample_encoding encoding{native_sample_encoding::unknown};
    std::uint16_t channels{};
    std::uint32_t sample_rate{};
    std::uint16_t bits_per_sample{};
    std::uint16_t valid_bits_per_sample{};
    std::uint16_t block_align{};
    std::uint32_t average_bytes_per_second{};
    std::uint32_t channel_mask{};
    std::uint16_t extension_size{};
    bool extensible{};

    friend constexpr bool operator==(const native_audio_format&,
        const native_audio_format&) noexcept = default;
};

enum class native_format_support : std::uint8_t {
    exact,
    closest,
};

struct native_format_support_result final {
    native_format_support support{native_format_support::exact};
    std::optional<native_audio_format> closest_format = std::nullopt;
};

enum class native_buffer_disposition : std::uint8_t {
    audio,
    silent,
};

enum class native_wait_outcome : std::uint8_t {
    buffer_ready,
    worker_wakeup,
    stop_requested,
    timeout,
    failed,
};

struct native_wait_result final {
    native_wait_outcome outcome{native_wait_outcome::failed};
    native_error error{.operation = native_operation::wait, .native_code = 0U};
};

struct native_call_status final {
    bool attempted{};
    bool succeeded{};
    std::uint32_t native_code{};
};

struct native_cleanup_report final {
    native_call_status stop{};
    native_call_status buffer_event_close{};
    native_call_status mmcss_revert{};
};

struct native_notification_result final {
    bool signaled{};
    native_error error{};
};

struct native_mmcss_registration final {
    bool registered{};
    std::uint32_t native_code{};
};

} // namespace inputnext::audio_detail
