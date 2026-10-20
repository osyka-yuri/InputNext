#include <inputnext/xinput.hpp>

#include "query_projection.hpp"
#include "service_state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/xinput_types.hpp>

#include <cstdint>
#include <optional>
#include <utility>

namespace inputnext::xinput {
namespace {

struct query_session final {
    assignment occupant;
    read_binding reader;
};

template <typename State>
[[nodiscard]] std::optional<query_session> capture_query_session(State& state, DWORD user_index) {
    const auto guard = state.focus.lock_presentation();
    if (state.closed) {
        return std::nullopt;
    }
    const auto& position = state.positions[user_index];
    if (!position.occupant.has_value()) {
        return std::nullopt;
    }
    return query_session{.occupant = *position.occupant, .reader = position.reader};
}

template <typename State>
[[nodiscard]] std::optional<metadata_snapshot> query_metadata(State& state,
    connection_target target) {
    try {
        auto queried = state.metadata.query(target);
        if (!queried.has_value() || queried->target != target) {
            return std::nullopt;
        }
        return std::move(*queried);
    } catch (...) {
        return std::nullopt;
    }
}

template <typename State>
[[nodiscard]] bool
final_query_capture(State& state, const query_session& session, std::optional<read_pin>& pin) {
    const auto guard = state.focus.lock_presentation();
    if (state.closed || session.occupant.user_index >= state.positions.size()) {
        return false;
    }
    const auto& position = state.positions[session.occupant.user_index];
    if (!position.occupant.has_value() || *position.occupant != session.occupant) {
        return false;
    }

    auto captured = session.reader.capture();
    if (!captured.has_value()) {
        return false;
    }
    pin.emplace(std::move(*captured));
    return pin->target() == session.occupant.target;
}

} // namespace

DWORD service::get_capabilities(flavor version,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): preserves the XInput entry-point order.
    DWORD user_index,
    DWORD flags,
    XINPUT_CAPABILITIES* capabilities) {
    if (flags != 0U && flags != XINPUT_FLAG_GAMEPAD) {
        return ERROR_BAD_ARGUMENTS;
    }
    if (user_index >= implementation_->positions.size() || capabilities == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }

    auto session = capture_query_session(*implementation_, user_index);
    if (!session.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    std::optional<metadata_snapshot> metadata;
    XINPUT_CAPABILITIES projected = detail::base_capabilities();
    if (version == flavor::v9_1_0) {
        projected.Vibration.wLeftMotorSpeed = 65535U;
        projected.Vibration.wRightMotorSpeed = 65535U;
    } else {
        metadata = query_metadata(*implementation_, session->occupant.target);
        if (!metadata.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        projected = detail::dynamic_capabilities(*metadata);
    }

    std::optional<read_pin> final_pin;
    if (!final_query_capture(*implementation_, *session, final_pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    *capabilities = projected;
    return ERROR_SUCCESS;
}

DWORD service::get_capabilities_ex(flavor /*version*/,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): preserves the XInput entry-point order.
    DWORD user_index,
    DWORD flags,
    capabilities_ex* capabilities) {
    if (flags != 0U && flags != XINPUT_FLAG_GAMEPAD) {
        return ERROR_BAD_ARGUMENTS;
    }
    if (user_index >= implementation_->positions.size() || capabilities == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }

    auto session = capture_query_session(*implementation_, user_index);
    if (!session.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    auto metadata = query_metadata(*implementation_, session->occupant.target);
    if (!metadata.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    const capabilities_ex projected = detail::extended_capabilities(*metadata);

    std::optional<read_pin> final_pin;
    if (!final_query_capture(*implementation_, *session, final_pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    *capabilities = projected;
    return ERROR_SUCCESS;
}

DWORD service::get_battery_information(flavor /*version*/,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): preserves the XInput entry-point order.
    DWORD user_index,
    BYTE device_type,
    XINPUT_BATTERY_INFORMATION* battery) {
    if (user_index >= implementation_->positions.size() || battery == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }
    if (device_type != BATTERY_DEVTYPE_GAMEPAD && device_type != BATTERY_DEVTYPE_HEADSET) {
        return ERROR_BAD_ARGUMENTS;
    }

    auto session = capture_query_session(*implementation_, user_index);
    if (!session.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    XINPUT_BATTERY_INFORMATION projected{};
    if (device_type == BATTERY_DEVTYPE_HEADSET) {
        projected.BatteryType = BATTERY_TYPE_DISCONNECTED;
        projected.BatteryLevel = BATTERY_LEVEL_EMPTY;
    } else {
        auto metadata = query_metadata(*implementation_, session->occupant.target);
        if (!metadata.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
        projected = detail::gamepad_battery(*metadata);
    }

    std::optional<read_pin> final_pin;
    if (!final_query_capture(*implementation_, *session, final_pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    *battery = projected;
    return ERROR_SUCCESS;
}

DWORD service::get_audio_device_ids(flavor version,
    DWORD user_index,
    LPWSTR render_id,
    UINT* render_count,
    LPWSTR capture_id,
    UINT* capture_count,
    std::uint32_t* write_flags) {
    if (version != flavor::v1_4) {
        return ERROR_NOT_SUPPORTED;
    }
    if (user_index >= implementation_->positions.size()) {
        return ERROR_BAD_ARGUMENTS;
    }

    auto session = capture_query_session(*implementation_, user_index);
    if (!session.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    std::optional<metadata_snapshot> metadata;
    if (render_count != nullptr || capture_count != nullptr) {
        metadata = query_metadata(*implementation_, session->occupant.target);
        if (!metadata.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
    }

    std::optional<read_pin> final_pin;
    if (!final_query_capture(*implementation_, *session, final_pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    if (write_flags != nullptr) {
        *write_flags = 0U;
    }
    const auto render_endpoint = metadata.has_value()
                                     ? detail::endpoint_id(metadata->headset_render)
                                     : std::u16string_view{};
    if (detail::write_endpoint(render_endpoint,
            render_id,
            render_count,
            0x01U,
            0x02U,
            write_flags) == detail::endpoint_write_result::insufficient_buffer) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    const auto capture_endpoint = metadata.has_value()
                                      ? detail::endpoint_id(metadata->headset_capture)
                                      : std::u16string_view{};
    if (detail::write_endpoint(capture_endpoint,
            capture_id,
            capture_count,
            0x04U,
            0x08U,
            write_flags) == detail::endpoint_write_result::insufficient_buffer) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    return ERROR_SUCCESS;
}

DWORD service::get_dsound_audio_device_guids(flavor version,
    DWORD user_index,
    GUID* render_guid,
    GUID* capture_guid) {
    if (version == flavor::v1_4) {
        return ERROR_NOT_SUPPORTED;
    }
    if (user_index >= implementation_->positions.size()) {
        return ERROR_BAD_ARGUMENTS;
    }

    auto session = capture_query_session(*implementation_, user_index);
    if (!session.has_value()) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    std::optional<metadata_snapshot> metadata;
    if (render_guid != nullptr || capture_guid != nullptr) {
        metadata = query_metadata(*implementation_, session->occupant.target);
        if (!metadata.has_value()) {
            return ERROR_DEVICE_NOT_CONNECTED;
        }
    }

    GUID render{};
    GUID capture{};
    if (metadata.has_value()) {
        render = detail::project_guid(metadata->headset_render.direct_sound_guid);
        capture = detail::project_guid(metadata->headset_capture.direct_sound_guid);
    }

    std::optional<read_pin> final_pin;
    if (!final_query_capture(*implementation_, *session, final_pin)) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (render_guid != nullptr) {
        *render_guid = render;
    }
    if (capture_guid != nullptr) {
        *capture_guid = capture;
    }
    return ERROR_SUCCESS;
}

} // namespace inputnext::xinput
