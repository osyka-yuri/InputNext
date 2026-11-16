#include "parameter_checks.hpp"

#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "dinput/profile.hpp"
#include "parameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <optional>
#include <variant>

namespace inputnext::dinput::force_feedback {
namespace {

using detail::complete_fields;
using detail::coordinate_mask;
using detail::has_parameter;
using detail::has_start_delay;
using detail::registered_effect;
using detail::selector_flags;
using detail::selector_mode;
using detail::supported_size;
using detail::valid_effect_flags;
using detail::validate_parameter_flags;

constexpr DWORD axes_and_direction =
    static_cast<DWORD>(DIEP_AXES) | static_cast<DWORD>(DIEP_DIRECTION);
constexpr DWORD optional_readback_fields =
    DIEP_ENVELOPE | DIEP_STARTDELAY | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL;

[[nodiscard]] std::optional<DWORD> find_offset(std::size_t profile_index,
    const compiled_format* format) noexcept {
    if (format == nullptr || format->binding_count > max_format_objects ||
        format->data_size == 0U || format->data_size > max_format_data_size) {
        return std::nullopt;
    }
    std::optional<DWORD> result;
    for (DWORD index = 0U; index < format->binding_count; ++index) {
        const auto& binding = format->bindings[index];
        if (binding.kind != object_kind::axis || binding.profile_object_index != profile_index ||
            binding.data_offset > format->data_size ||
            sizeof(LONG) > format->data_size - binding.data_offset ||
            binding.data_offset % alignof(DWORD) != 0U || result.has_value()) {
            if (binding.kind == object_kind::axis &&
                binding.profile_object_index == profile_index) {
                return std::nullopt;
            }
            continue;
        }
        result = binding.data_offset;
    }
    return result;
}

[[nodiscard]] DWORD force_size(const effect_descriptor& effect) noexcept {
    switch (effect.wave) {
    case waveform::constant:
        return static_cast<DWORD>(sizeof(DICONSTANTFORCE));
    case waveform::ramp:
        return static_cast<DWORD>(sizeof(DIRAMPFORCE));
    case waveform::square:
    case waveform::sine:
    case waveform::triangle:
    case waveform::sawtooth_up:
    case waveform::sawtooth_down:
        return static_cast<DWORD>(sizeof(DIPERIODIC));
    }
    return 0U;
}

[[nodiscard]] bool force_matches(const effect_parameters& parameters,
    const effect_descriptor& effect) noexcept {
    switch (effect.wave) {
    case waveform::constant:
        return std::holds_alternative<DICONSTANTFORCE>(parameters.force);
    case waveform::ramp:
        return std::holds_alternative<DIRAMPFORCE>(parameters.force);
    case waveform::square:
    case waveform::sine:
    case waveform::triangle:
    case waveform::sawtooth_up:
    case waveform::sawtooth_down:
        return std::holds_alternative<DIPERIODIC>(parameters.force);
    }
    return false;
}

[[nodiscard]] HRESULT axis_selectors_for_readback(const effect_parameters& source,
    DWORD flags,
    const compiled_format* format,
    std::array<DWORD, 2U>& selectors) noexcept {
    const auto mode = selector_flags(flags);
    if (!mode.has_value()) {
        return DIERR_INVALIDPARAM;
    }
    const auto profile = objects();
    for (DWORD index = 0U; index < source.axis_count; ++index) {
        const auto object_index = source.axes[index];
        if (object_index >= profile.size() || !actuator(object_index)) {
            return DIERR_INCOMPLETEEFFECT;
        }
        if (*mode == selector_mode::object_ids) {
            selectors[index] = profile[object_index].id | DIDFT_FFACTUATOR;
            continue;
        }
        const auto offset = find_offset(object_index, format);
        if (!offset.has_value()) {
            return DIERR_OBJECTNOTFOUND;
        }
        selectors[index] = *offset;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT validate_get_fields(const effect_parameters& source,
    const effect_descriptor& effect,
    DWORD selected) noexcept {
    const auto required = selected & ~optional_readback_fields;
    if (!complete_fields(source.valid, required) ||
        (has_parameter(selected, DIEP_ENVELOPE) && !complete_fields(source.valid, DIEP_ENVELOPE)) ||
        (has_parameter(selected, DIEP_STARTDELAY) &&
            !complete_fields(source.valid, DIEP_STARTDELAY)) ||
        (has_parameter(selected, DIEP_AXES) && source.axis_count == 0U) ||
        (has_parameter(selected, DIEP_DIRECTION) && source.direction_count == 0U) ||
        (has_parameter(selected, DIEP_TYPESPECIFICPARAMS) && !force_matches(source, effect))) {
        return DIERR_INCOMPLETEEFFECT;
    }
    return DI_OK;
}

struct direction_readback final {
    DWORD coordinate_flags{};
    std::array<LONG, 2U> values{};
};

[[nodiscard]] LONG scaled_direction_component(double component) noexcept {
    return static_cast<LONG>(std::round(component * static_cast<double>(DI_FFNOMINALMAX)));
}

[[nodiscard]] HRESULT prepare_direction_readback(const effect_parameters& source,
    DWORD flags,
    direction_readback& destination) noexcept {
    const auto requested = flags & coordinate_mask;
    if (requested == 0U || source.direction_count != source.axis_count ||
        source.direction_count == 0U || source.direction_count > source.directions.size()) {
        return DIERR_INVALIDPARAM;
    }
    if ((source.coordinate_flags != DIEFF_CARTESIAN && source.coordinate_flags != DIEFF_POLAR) ||
        (source.coordinate_flags == DIEFF_POLAR &&
            (source.direction_count != 2U || source.directions[0U] < 0 ||
                source.directions[0U] >= 36000 || source.directions[1U] != 0))) {
        return DIERR_INCOMPLETEEFFECT;
    }

    if ((requested & source.coordinate_flags) != 0U) {
        destination.coordinate_flags = source.coordinate_flags;
    } else if ((requested & DIEFF_CARTESIAN) != 0U) {
        destination.coordinate_flags = DIEFF_CARTESIAN;
    } else if ((requested & DIEFF_POLAR) != 0U) {
        destination.coordinate_flags = DIEFF_POLAR;
    } else {
        return DIERR_UNSUPPORTED;
    }

    if (destination.coordinate_flags == source.coordinate_flags) {
        std::copy_n(source.directions.begin(), source.direction_count, destination.values.begin());
        return DI_OK;
    }
    if (source.direction_count != 2U) {
        return DIERR_UNSUPPORTED;
    }

    constexpr auto degrees_per_radian = 18000.0 / std::numbers::pi_v<double>;
    constexpr auto radians_per_degree = std::numbers::pi_v<double> / 18000.0;
    if (source.coordinate_flags == DIEFF_CARTESIAN && destination.coordinate_flags == DIEFF_POLAR) {
        const auto x = static_cast<double>(source.directions[0U]);
        const auto y = static_cast<double>(source.directions[1U]);
        if (x == 0.0 && y == 0.0) {
            return DIERR_INVALIDPARAM;
        }
        auto angle = std::atan2(x, -y) * degrees_per_radian;
        if (angle < 0.0) {
            angle += 36000.0;
        }
        auto rounded_angle = std::round(angle);
        if (rounded_angle >= 36000.0) {
            rounded_angle = 0.0;
        }
        destination.values[0U] = static_cast<LONG>(rounded_angle);
        destination.values[1U] = 0;
        return DI_OK;
    }
    if (source.coordinate_flags == DIEFF_POLAR && destination.coordinate_flags == DIEFF_CARTESIAN) {
        const auto angle = static_cast<double>(source.directions[0U]) * radians_per_degree;
        destination.values[0U] = scaled_direction_component(std::sin(angle));
        destination.values[1U] = scaled_direction_component(-std::cos(angle));
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

[[nodiscard]] HRESULT prepare_get_buffers(const effect_parameters& source,
    const effect_descriptor& effect,
    DIEFFECT& destination,
    DWORD selected,
    const compiled_format* format,
    const direction_readback& direction,
    std::array<DWORD, 2U>& selectors) noexcept {
    if (has_parameter(selected, DIEP_AXES)) {
        const auto result =
            axis_selectors_for_readback(source, destination.dwFlags, format, selectors);
        if (FAILED(result)) {
            return result;
        }
    }
    if (has_parameter(selected, DIEP_DIRECTION) &&
        (direction.coordinate_flags == 0U || source.direction_count != source.axis_count)) {
        return DIERR_UNSUPPORTED;
    }
    if (has_parameter(selected, DIEP_ENVELOPE) && source.envelope.has_value() &&
        (destination.lpEnvelope == nullptr ||
            destination.lpEnvelope->dwSize != sizeof(DIENVELOPE))) {
        return DIERR_INVALIDPARAM;
    }

    const auto required_axes = static_cast<DWORD>(source.axis_count);
    const auto required_force = force_size(effect);
    const bool axes_requested = has_parameter(selected, axes_and_direction);
    const bool axes_short = axes_requested && destination.cAxes < required_axes;
    const bool force_requested = has_parameter(selected, DIEP_TYPESPECIFICPARAMS);
    const bool force_short = force_requested && destination.cbTypeSpecificParams < required_force;
    if (!axes_short && axes_requested &&
        ((has_parameter(selected, DIEP_AXES) && destination.rgdwAxes == nullptr) ||
            (has_parameter(selected, DIEP_DIRECTION) && destination.rglDirection == nullptr))) {
        return DIERR_INVALIDPARAM;
    }
    if (!force_short && force_requested && destination.lpvTypeSpecificParams == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    if (axes_short) {
        destination.cAxes = required_axes;
    }
    if (force_short) {
        destination.cbTypeSpecificParams = required_force;
    }
    return axes_short || force_short ? DIERR_MOREDATA : DI_OK;
}

[[nodiscard]] HRESULT write_type_specific(DIEFFECT& destination,
    const effect_parameters& source) noexcept {
    const auto copy_payload = [&destination](const auto* force) noexcept {
        if (force == nullptr) {
            return false;
        }
        std::memcpy(destination.lpvTypeSpecificParams, force, sizeof(*force));
        destination.cbTypeSpecificParams = static_cast<DWORD>(sizeof(*force));
        return true;
    };
    if (const auto* constant = std::get_if<DICONSTANTFORCE>(&source.force); constant != nullptr) {
        return copy_payload(constant) ? DI_OK : DIERR_INCOMPLETEEFFECT;
    }
    if (const auto* ramp = std::get_if<DIRAMPFORCE>(&source.force); ramp != nullptr) {
        return copy_payload(ramp) ? DI_OK : DIERR_INCOMPLETEEFFECT;
    }
    if (const auto* periodic = std::get_if<DIPERIODIC>(&source.force); periodic != nullptr) {
        return copy_payload(periodic) ? DI_OK : DIERR_INCOMPLETEEFFECT;
    }
    return DIERR_INCOMPLETEEFFECT;
}

[[nodiscard]] HRESULT write_selected_fields(DIEFFECT& destination,
    const effect_parameters& source,
    DWORD selected,
    const direction_readback& direction,
    const std::array<DWORD, 2U>& selectors) noexcept {
    if (has_parameter(selected, DIEP_DURATION)) {
        destination.dwDuration = source.duration;
    }
    if (has_parameter(selected, DIEP_SAMPLEPERIOD)) {
        destination.dwSamplePeriod = source.sample_period;
    }
    if (has_parameter(selected, DIEP_GAIN)) {
        destination.dwGain = source.gain;
    }
    if (has_parameter(selected, DIEP_TRIGGERBUTTON)) {
        destination.dwTriggerButton = source.trigger_button;
    }
    if (has_parameter(selected, DIEP_TRIGGERREPEATINTERVAL)) {
        destination.dwTriggerRepeatInterval = source.trigger_repeat_interval;
    }
    if (has_parameter(selected, DIEP_AXES)) {
        std::copy_n(selectors.begin(), source.axis_count, destination.rgdwAxes);
    }
    if (has_parameter(selected, DIEP_DIRECTION)) {
        std::copy_n(direction.values.begin(), source.direction_count, destination.rglDirection);
        destination.dwFlags = (destination.dwFlags & ~coordinate_mask) | direction.coordinate_flags;
    }
    if (has_parameter(selected, DIEP_ENVELOPE)) {
        if (!source.envelope.has_value()) {
            destination.lpEnvelope = nullptr;
        } else {
            *destination.lpEnvelope = *source.envelope;
        }
    }
    if (has_parameter(selected, DIEP_TYPESPECIFICPARAMS)) {
        const auto result = write_type_specific(destination, source);
        if (FAILED(result)) {
            return result;
        }
    }
    if (has_parameter(selected, DIEP_STARTDELAY)) {
        destination.dwStartDelay = source.start_delay;
    }
    return DI_OK;
}

} // namespace

HRESULT get_parameters(const effect_parameters& source,
    const effect_descriptor& candidate,
    DIEFFECT* destination,
    DWORD selected,
    const compiled_format* format) noexcept {
    const auto* effect = registered_effect(candidate);
    if (effect == nullptr) {
        return DIERR_UNSUPPORTED;
    }
    if (destination == nullptr || !supported_size(destination->dwSize) ||
        !valid_effect_flags(destination->dwFlags)) {
        return DIERR_INVALIDPARAM;
    }
    const auto validation_flags =
        effect->wave == waveform::ramp ? selected & ~static_cast<DWORD>(DIEP_ENVELOPE) : selected;
    const auto valid_flags = validate_parameter_flags(validation_flags, *effect);
    if (FAILED(valid_flags)) {
        return valid_flags;
    }
    if (has_parameter(selected, DIEP_STARTDELAY) && !has_start_delay(destination->dwSize)) {
        return DIERR_INVALIDPARAM;
    }
    const auto complete_result = validate_get_fields(source, *effect, selected);
    if (FAILED(complete_result)) {
        return complete_result;
    }
    std::array<DWORD, 2U> selectors{};
    direction_readback direction{};
    if (has_parameter(selected, DIEP_DIRECTION)) {
        const auto direction_result =
            prepare_direction_readback(source, destination->dwFlags, direction);
        if (FAILED(direction_result)) {
            return direction_result;
        }
    }
    const auto buffer_result =
        prepare_get_buffers(source, *effect, *destination, selected, format, direction, selectors);
    if (FAILED(buffer_result)) {
        return buffer_result;
    }
    const auto write_result =
        write_selected_fields(*destination, source, selected, direction, selectors);
    if (SUCCEEDED(write_result) && has_parameter(selected, axes_and_direction)) {
        destination->cAxes = static_cast<DWORD>(source.axis_count);
    }
    return write_result;
}

} // namespace inputnext::dinput::force_feedback
