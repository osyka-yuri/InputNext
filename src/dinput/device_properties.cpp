#include "device_properties.hpp"

#include "device_info.hpp"
#include "format.hpp"
#include "native_types.hpp"
#include "profile.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>

namespace inputnext::dinput {
namespace {

using setting = property_change::setting;

template <typename Property>
[[nodiscard]] Property copy_property(const DIPROPHEADER* header) noexcept {
    Property result{};
    std::memcpy(&result, header, sizeof(result));
    return result;
}

template <typename Property>
void write_property(DIPROPHEADER* header, const Property& value) noexcept {
    std::memcpy(header, &value, sizeof(value));
}

[[nodiscard]] std::optional<std::size_t> find_axis(const DIPROPHEADER& header,
    const compiled_format* format,
    const device_model& model) noexcept {
    const auto selected = find_object(header.dwObj, header.dwHow, format, model);
    if (!selected.has_value()) {
        return std::nullopt;
    }
    const auto& object = model.objects()[*selected];
    return object.kind == object_kind::axis ? std::optional{object.source_index} : std::nullopt;
}

[[nodiscard]] HRESULT read_axis_property(const GUID* token,
    DIPROPHEADER* header,
    const compiled_format* format,
    const axis_configuration& axes,
    const device_model& model) noexcept {
    const bool range = token == &DIPROP_RANGE;
    if (header->dwHow == DIPH_DEVICE ||
        header->dwSize != (range ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD))) {
        return DIERR_INVALIDPARAM;
    }
    const auto axis = find_axis(*header, format, model);
    if (!axis.has_value()) {
        return DIERR_OBJECTNOTFOUND;
    }
    const auto& settings = axes[*axis];
    if (range) {
        auto value = copy_property<DIPROPRANGE>(header);
        value.lMin = settings.minimum;
        value.lMax = settings.maximum;
        write_property(header, value);
        return DI_OK;
    }
    auto value = copy_property<DIPROPDWORD>(header);
    if (token == &DIPROP_DEADZONE) {
        value.dwData = settings.deadzone;
    } else if (token == &DIPROP_SATURATION) {
        value.dwData = settings.saturation;
    } else {
        value.dwData = 1U;
    }
    write_property(header, value);
    return DI_OK;
}

[[nodiscard]] HRESULT read_device_dword(const GUID* token,
    DIPROPHEADER* header,
    const device_identity& identity,
    DWORD requested_buffer_size) noexcept {
    if (header->dwSize != sizeof(DIPROPDWORD) || header->dwHow != DIPH_DEVICE) {
        return DIERR_INVALIDPARAM;
    }
    auto value = copy_property<DIPROPDWORD>(header);
    if (token == &DIPROP_VIDPID) {
        if (!identity.vendor_product.has_value()) {
            return DIERR_UNSUPPORTED;
        }
        value.dwData = *identity.vendor_product;
    } else {
        value.dwData = token == &DIPROP_AXISMODE ? DIPROPAXISMODE_ABS : requested_buffer_size;
    }
    write_property(header, value);
    return DI_OK;
}

[[nodiscard]] HRESULT read_identity_property(const GUID* token,
    DIPROPHEADER* header,
    const device_identity& identity) noexcept {
    if (token == &DIPROP_GUIDANDPATH) {
        if (header->dwSize != sizeof(DIPROPGUIDANDPATH) || header->dwHow != DIPH_DEVICE) {
            return DIERR_INVALIDPARAM;
        }
        if (!identity.path.has_value()) {
            return DIERR_UNSUPPORTED;
        }
        auto value = *identity.path;
        value.diph = *header;
        write_property(header, value);
        return DI_OK;
    }
    if (token == &DIPROP_INSTANCENAME || token == &DIPROP_PRODUCTNAME) {
        if (header->dwSize != sizeof(DIPROPSTRING) || header->dwHow != DIPH_DEVICE) {
            return DIERR_INVALIDPARAM;
        }
        auto value = copy_property<DIPROPSTRING>(header);
        const auto* name = token == &DIPROP_INSTANCENAME ? identity.instance.tszInstanceName
                                                         : identity.instance.tszProductName;
        std::copy_n(name, MAX_PATH, value.wsz);
        write_property(header, value);
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

[[nodiscard]] std::optional<setting> writable_setting(const GUID* token) noexcept {
    if (token == &DIPROP_RANGE) {
        return setting::range;
    }
    if (token == &DIPROP_DEADZONE) {
        return setting::deadzone;
    }
    if (token == &DIPROP_SATURATION) {
        return setting::saturation;
    }
    if (token == &DIPROP_AXISMODE) {
        return setting::axis_mode;
    }
    if (token == &DIPROP_BUFFERSIZE) {
        return setting::buffer_size;
    }
    return std::nullopt;
}

void apply_axis_change(axis_properties& axis, const property_change& change) noexcept {
    switch (change.kind) {
    case setting::range:
        axis.minimum = change.minimum;
        axis.maximum = change.maximum;
        break;
    case setting::deadzone:
        axis.deadzone = change.value;
        break;
    case setting::saturation:
        axis.saturation = change.value;
        break;
    case setting::axis_mode:
    case setting::buffer_size:
        break;
    }
}

} // namespace

bool valid_property_header(const DIPROPHEADER& header) noexcept {
    return header.dwHeaderSize == sizeof(DIPROPHEADER) && header.dwHow <= DIPH_BYUSAGE &&
           (header.dwHow != DIPH_DEVICE || header.dwObj == 0U);
}

HRESULT read_device_property(const GUID* token,
    DIPROPHEADER* header,
    const device_identity& identity,
    const compiled_format* format,
    const axis_configuration& axes,
    DWORD requested_buffer_size,
    const device_model& model) noexcept {
    if (token == &DIPROP_RANGE || token == &DIPROP_DEADZONE || token == &DIPROP_SATURATION ||
        token == &DIPROP_GRANULARITY) {
        return read_axis_property(token, header, format, axes, model);
    }
    if (token == &DIPROP_AXISMODE || token == &DIPROP_BUFFERSIZE || token == &DIPROP_VIDPID) {
        return read_device_dword(token, header, identity, requested_buffer_size);
    }
    return read_identity_property(token, header, identity);
}

HRESULT decode_property_change(const GUID* token,
    const DIPROPHEADER& header,
    property_change& change) noexcept {
    const auto kind = writable_setting(token);
    if (!kind.has_value()) {
        return DIERR_UNSUPPORTED;
    }
    change.kind = *kind;
    const bool range = change.kind == setting::range;
    if (header.dwSize != (range ? sizeof(DIPROPRANGE) : sizeof(DIPROPDWORD))) {
        return DIERR_INVALIDPARAM;
    }
    if (range) {
        const auto limits = copy_property<DIPROPRANGE>(&header);
        change.minimum = limits.lMin;
        change.maximum = limits.lMax;
        return limits.lMax > limits.lMin ? DI_OK : DIERR_INVALIDPARAM;
    }
    change.value = copy_property<DIPROPDWORD>(&header).dwData;
    if ((change.kind == setting::deadzone || change.kind == setting::saturation) &&
        change.value > 10000U) {
        return DIERR_INVALIDPARAM;
    }
    if (change.kind == setting::axis_mode && change.value > DIPROPAXISMODE_REL) {
        return DIERR_INVALIDPARAM;
    }
    return !change.affects_axes() && header.dwHow != DIPH_DEVICE ? DIERR_INVALIDPARAM : DI_OK;
}

HRESULT apply_property_change(const property_change& change,
    const DIPROPHEADER& header,
    const compiled_format* format,
    axis_configuration& candidate,
    const device_model& model) noexcept {
    if (!change.affects_axes()) {
        return change.kind == setting::buffer_size || change.value == DIPROPAXISMODE_ABS
                   ? DI_OK
                   : DIERR_UNSUPPORTED;
    }
    if (header.dwHow == DIPH_DEVICE) {
        for (auto& axis : candidate) {
            apply_axis_change(axis, change);
        }
        return DI_OK;
    }
    const auto axis = find_axis(header, format, model);
    if (!axis.has_value()) {
        return DIERR_OBJECTNOTFOUND;
    }
    apply_axis_change(candidate[*axis], change);
    return DI_OK;
}

} // namespace inputnext::dinput
