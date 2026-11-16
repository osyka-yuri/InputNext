#include "parameter_checks.hpp"

#include "dinput/format.hpp"
#include "dinput/native_types.hpp"
#include "parameters.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <variant>

namespace inputnext::dinput::force_feedback {

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

const effect_descriptor* detail::registered_effect(const effect_descriptor& candidate) noexcept {
    if (candidate.id == nullptr) {
        return nullptr;
    }
    const auto* known = find_effect(*candidate.id);
    if (known == nullptr || known->wave != candidate.wave || known->type != candidate.type) {
        return nullptr;
    }
    return known;
}

namespace {

constexpr DWORD common_dynamic_params =
    DIEP_DURATION | DIEP_SAMPLEPERIOD | DIEP_GAIN | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS;
constexpr DWORD common_static_params =
    DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL | DIEP_AXES | DIEP_STARTDELAY;
constexpr DWORD max_force = DI_FFNOMINALMAX;
constexpr LONG signed_max_force = static_cast<LONG>(DI_FFNOMINALMAX);
constexpr std::int64_t wide_max_force = static_cast<std::int64_t>(DI_FFNOMINALMAX);

constexpr std::array<effect_descriptor, 7U> effect_catalogue{{
    {.id = &GUID_ConstantForce,
        .wave = waveform::constant,
        .name = "Constant Force",
        .wide_name = L"Constant Force",
        .type = DIEFT_CONSTANTFORCE | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
    {.id = &GUID_RampForce,
        .wave = waveform::ramp,
        .name = "Ramp Force",
        .wide_name = L"Ramp Force",
        .type = DIEFT_RAMPFORCE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params},
    {.id = &GUID_Square,
        .wave = waveform::square,
        .name = "Square",
        .wide_name = L"Square",
        .type = DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
    {.id = &GUID_Sine,
        .wave = waveform::sine,
        .name = "Sine",
        .wide_name = L"Sine",
        .type = DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
    {.id = &GUID_Triangle,
        .wave = waveform::triangle,
        .name = "Triangle",
        .wide_name = L"Triangle",
        .type = DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
    {.id = &GUID_SawtoothUp,
        .wave = waveform::sawtooth_up,
        .name = "Sawtooth Up",
        .wide_name = L"Sawtooth Up",
        .type = DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
    {.id = &GUID_SawtoothDown,
        .wave = waveform::sawtooth_down,
        .name = "Sawtooth Down",
        .wide_name = L"Sawtooth Down",
        .type = DIEFT_PERIODIC | DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY,
        .static_params = common_static_params,
        .dynamic_params = common_dynamic_params | DIEP_ENVELOPE},
}};

[[nodiscard]] std::optional<DWORD> single_coordinate_flag(DWORD effect_flags) noexcept {
    const auto flags = effect_flags & coordinate_mask;
    if (flags == DIEFF_CARTESIAN || flags == DIEFF_POLAR) {
        return flags;
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_set_effect_flags(DWORD flags) noexcept {
    return valid_effect_flags(flags) && std::popcount(flags & coordinate_mask) <= 1;
}

[[nodiscard]] HRESULT resolve_axis(DWORD selector,
    selector_mode mode,
    const compiled_format* format,
    std::size_t& profile_index) noexcept {
    const DWORD how = mode == selector_mode::object_ids ? static_cast<DWORD>(DIPH_BYID)
                                                        : static_cast<DWORD>(DIPH_BYOFFSET);
    const auto resolved = find_object(selector, how, format);
    if (!resolved.has_value() || !actuator(*resolved)) {
        return DIERR_OBJECTNOTFOUND;
    }
    profile_index = *resolved;
    return DI_OK;
}

[[nodiscard]] HRESULT read_force(const effect_descriptor& effect,
    const DIEFFECT& source,
    std::variant<DICONSTANTFORCE, DIRAMPFORCE, DIPERIODIC>& destination) noexcept {
    if (source.lpvTypeSpecificParams == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    switch (effect.wave) {
    case waveform::constant: {
        if (source.cbTypeSpecificParams != sizeof(DICONSTANTFORCE)) {
            return DIERR_INVALIDPARAM;
        }
        DICONSTANTFORCE value{};
        std::memcpy(&value, source.lpvTypeSpecificParams, sizeof(value));
        const auto magnitude = static_cast<std::int64_t>(value.lMagnitude);
        if (magnitude < -wide_max_force || magnitude > wide_max_force) {
            return DIERR_INVALIDPARAM;
        }
        destination = value;
        return DI_OK;
    }
    case waveform::ramp: {
        if (source.cbTypeSpecificParams != sizeof(DIRAMPFORCE)) {
            return DIERR_INVALIDPARAM;
        }
        DIRAMPFORCE value{};
        std::memcpy(&value, source.lpvTypeSpecificParams, sizeof(value));
        if (value.lStart < -signed_max_force || value.lStart > signed_max_force ||
            value.lEnd < -signed_max_force || value.lEnd > signed_max_force) {
            return DIERR_INVALIDPARAM;
        }
        destination = value;
        return DI_OK;
    }
    case waveform::square:
    case waveform::sine:
    case waveform::triangle:
    case waveform::sawtooth_up:
    case waveform::sawtooth_down: {
        if (source.cbTypeSpecificParams != sizeof(DIPERIODIC)) {
            return DIERR_INVALIDPARAM;
        }
        DIPERIODIC value{};
        std::memcpy(&value, source.lpvTypeSpecificParams, sizeof(value));
        if (value.dwMagnitude > max_force || value.dwPeriod == 0U || value.dwPhase >= 36000U ||
            value.lOffset < -signed_max_force || value.lOffset > signed_max_force) {
            return DIERR_INVALIDPARAM;
        }
        destination = value;
        return DI_OK;
    }
    }
    return DIERR_UNSUPPORTED;
}

[[nodiscard]] bool valid_envelope(const DIENVELOPE& envelope) noexcept {
    return envelope.dwSize == sizeof(DIENVELOPE) && envelope.dwAttackLevel <= max_force &&
           envelope.dwFadeLevel <= max_force;
}

[[nodiscard]] HRESULT stage_scalar_parameters(effect_parameters& staged,
    const effect_descriptor& effect,
    const DIEFFECT& source,
    DWORD selected) noexcept {
    if (has_parameter(selected, DIEP_DURATION)) {
        if (source.dwDuration == 0U ||
            (effect.wave == waveform::ramp && source.dwDuration == INFINITE)) {
            return DIERR_INVALIDPARAM;
        }
        staged.duration = source.dwDuration;
        staged.valid |= DIEP_DURATION;
    }
    if (has_parameter(selected, DIEP_SAMPLEPERIOD)) {
        if (source.dwSamplePeriod == INFINITE) {
            return DIERR_INVALIDPARAM;
        }
        staged.sample_period = source.dwSamplePeriod;
        staged.valid |= DIEP_SAMPLEPERIOD;
    }
    if (has_parameter(selected, DIEP_GAIN)) {
        if (source.dwGain > DI_FFNOMINALMAX) {
            return DIERR_INVALIDPARAM;
        }
        staged.gain = source.dwGain;
        staged.valid |= DIEP_GAIN;
    }
    if (has_parameter(selected, DIEP_TRIGGERBUTTON)) {
        if (source.dwTriggerButton != DIEB_NOTRIGGER) {
            return DIERR_UNSUPPORTED;
        }
        staged.trigger_button = source.dwTriggerButton;
        staged.valid |= DIEP_TRIGGERBUTTON;
    }
    if (has_parameter(selected, DIEP_TRIGGERREPEATINTERVAL)) {
        staged.trigger_repeat_interval = source.dwTriggerRepeatInterval;
        staged.valid |= DIEP_TRIGGERREPEATINTERVAL;
    }
    if (has_parameter(selected, DIEP_STARTDELAY)) {
        if (source.dwStartDelay == INFINITE) {
            return DIERR_INVALIDPARAM;
        }
        staged.start_delay = source.dwStartDelay;
        staged.valid |= DIEP_STARTDELAY;
    }
    return DI_OK;
}

[[nodiscard]] HRESULT stage_axes(effect_parameters& staged,
    const DIEFFECT& source,
    DWORD selected,
    const compiled_format* format) noexcept {
    if (!has_parameter(selected, DIEP_AXES)) {
        return DI_OK;
    }
    if (source.cAxes == 0U || source.cAxes > staged.axes.size() || source.rgdwAxes == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    const auto mode = selector_flags(source.dwFlags);
    if (!mode.has_value()) {
        return DIERR_INVALIDPARAM;
    }

    std::array<std::size_t, 2U> axes{};
    for (DWORD index = 0U; index < source.cAxes; ++index) {
        const auto result = resolve_axis(source.rgdwAxes[index], *mode, format, axes[index]);
        if (FAILED(result)) {
            return result;
        }
        if (index != 0U && axes[index] == axes[0U]) {
            return DIERR_INVALIDPARAM;
        }
    }
    if (staged.direction_count != 0U && staged.direction_count != source.cAxes &&
        !has_parameter(selected, DIEP_DIRECTION)) {
        return DIERR_INVALIDPARAM;
    }
    staged.axis_count = source.cAxes;
    staged.axes = axes;
    staged.valid |= DIEP_AXES;
    return DI_OK;
}

[[nodiscard]] HRESULT
stage_direction(effect_parameters& staged, const DIEFFECT& source, DWORD selected) noexcept {
    if (!has_parameter(selected, DIEP_DIRECTION)) {
        return DI_OK;
    }
    if (source.cAxes == 0U || source.cAxes > staged.directions.size() ||
        source.rglDirection == nullptr) {
        return DIERR_INVALIDPARAM;
    }
    const auto coordinate = single_coordinate_flag(source.dwFlags);
    if (!coordinate.has_value()) {
        return DIERR_UNSUPPORTED;
    }
    if (staged.axis_count != 0U && staged.axis_count != source.cAxes) {
        return DIERR_INVALIDPARAM;
    }
    if (*coordinate == DIEFF_POLAR &&
        (source.cAxes != 2U || source.rglDirection[0U] < 0 || source.rglDirection[0U] >= 36000 ||
            source.rglDirection[1U] != 0)) {
        return DIERR_INVALIDPARAM;
    }
    staged.direction_count = source.cAxes;
    staged.coordinate_flags = *coordinate;
    std::copy_n(source.rglDirection, source.cAxes, staged.directions.begin());
    staged.valid |= DIEP_DIRECTION;
    return DI_OK;
}

[[nodiscard]] HRESULT stage_envelope(effect_parameters& staged,
    const effect_descriptor& effect,
    const DIEFFECT& source,
    DWORD selected) noexcept {
    if (!has_parameter(selected, DIEP_ENVELOPE)) {
        return DI_OK;
    }
    if (effect.wave == waveform::ramp && source.lpEnvelope != nullptr) {
        return DIERR_UNSUPPORTED;
    }
    if (source.lpEnvelope != nullptr && !valid_envelope(*source.lpEnvelope)) {
        return DIERR_INVALIDPARAM;
    }
    if (source.lpEnvelope == nullptr) {
        staged.envelope.reset();
    } else {
        staged.envelope = *source.lpEnvelope;
    }
    staged.valid |= DIEP_ENVELOPE;
    return DI_OK;
}

[[nodiscard]] HRESULT stage_type_specific(effect_parameters& staged,
    const effect_descriptor& effect,
    const DIEFFECT& source,
    DWORD selected) noexcept {
    if (!has_parameter(selected, DIEP_TYPESPECIFICPARAMS)) {
        return DI_OK;
    }
    auto force = staged.force;
    const auto result = read_force(effect, source, force);
    if (FAILED(result)) {
        return result;
    }
    staged.force = force;
    staged.valid |= DIEP_TYPESPECIFICPARAMS;
    return DI_OK;
}

[[nodiscard]] bool valid_force(const effect_parameters& parameters) noexcept {
    if (const auto* constant = std::get_if<DICONSTANTFORCE>(&parameters.force);
        constant != nullptr) {
        return constant->lMagnitude >= -signed_max_force &&
               constant->lMagnitude <= signed_max_force;
    }
    if (const auto* ramp = std::get_if<DIRAMPFORCE>(&parameters.force); ramp != nullptr) {
        return ramp->lStart >= -signed_max_force && ramp->lStart <= signed_max_force &&
               ramp->lEnd >= -signed_max_force && ramp->lEnd <= signed_max_force &&
               parameters.duration != INFINITE;
    }
    if (const auto* periodic = std::get_if<DIPERIODIC>(&parameters.force); periodic != nullptr) {
        return periodic->dwMagnitude <= max_force && periodic->dwPeriod != 0U &&
               periodic->dwPhase < 36000U && periodic->lOffset >= -signed_max_force &&
               periodic->lOffset <= signed_max_force;
    }
    return false;
}

} // namespace

std::span<const effect_descriptor> effects() noexcept {
    return effect_catalogue;
}

const effect_descriptor* find_effect(REFGUID id) noexcept {
    const auto found =
        std::ranges::find_if(effect_catalogue, [&id](const effect_descriptor& effect) {
            return effect.id != nullptr && *effect.id == id;
        });
    return found == effect_catalogue.end() ? nullptr : &*found;
}

bool actuator(std::size_t profile_object_index) noexcept {
    return profile_object_index < 2U;
}

HRESULT set_parameters(effect_parameters& destination,
    const effect_descriptor& candidate,
    const DIEFFECT* source,
    DWORD selected,
    const compiled_format* format) noexcept {
    const auto* effect = registered_effect(candidate);
    if (effect == nullptr) {
        return DIERR_UNSUPPORTED;
    }
    if (source == nullptr || !supported_size(source->dwSize) ||
        !valid_set_effect_flags(source->dwFlags)) {
        return DIERR_INVALIDPARAM;
    }
    const auto validation_flags = (selected & DIEP_ENVELOPE) != 0U &&
                                          effect->wave == waveform::ramp &&
                                          source->lpEnvelope == nullptr
                                      ? selected & ~static_cast<DWORD>(DIEP_ENVELOPE)
                                      : selected;
    const auto valid_flags = validate_parameter_flags(validation_flags, *effect);
    if (FAILED(valid_flags)) {
        return valid_flags;
    }
    if (has_parameter(selected, DIEP_STARTDELAY) && !has_start_delay(source->dwSize)) {
        return DIERR_INVALIDPARAM;
    }

    auto staged = destination;
    const auto basic_result = stage_scalar_parameters(staged, *effect, *source, selected);
    if (FAILED(basic_result)) {
        return basic_result;
    }
    const auto axes_result = stage_axes(staged, *source, selected, format);
    if (FAILED(axes_result)) {
        return axes_result;
    }
    const auto direction_result = stage_direction(staged, *source, selected);
    if (FAILED(direction_result)) {
        return direction_result;
    }
    const auto envelope_result = stage_envelope(staged, *effect, *source, selected);
    if (FAILED(envelope_result)) {
        return envelope_result;
    }
    const auto force_result = stage_type_specific(staged, *effect, *source, selected);
    if (FAILED(force_result)) {
        return force_result;
    }
    destination = staged;
    return DI_OK;
}

bool complete(const effect_parameters& parameters) noexcept {
    constexpr DWORD required = DIEP_DURATION | DIEP_AXES | DIEP_DIRECTION |
                               DIEP_TYPESPECIFICPARAMS | DIEP_GAIN | DIEP_SAMPLEPERIOD;
    if (!complete_fields(parameters.valid, required) || parameters.duration == 0U ||
        parameters.gain > DI_FFNOMINALMAX || parameters.sample_period == INFINITE ||
        parameters.trigger_button != DIEB_NOTRIGGER || parameters.axis_count == 0U ||
        parameters.axis_count > parameters.axes.size() ||
        parameters.direction_count != parameters.axis_count ||
        (parameters.coordinate_flags != DIEFF_CARTESIAN &&
            parameters.coordinate_flags != DIEFF_POLAR) ||
        (parameters.coordinate_flags == DIEFF_POLAR && parameters.axis_count != 2U)) {
        return false;
    }
    for (DWORD index = 0U; index < parameters.axis_count; ++index) {
        if (!actuator(parameters.axes[index]) ||
            (index != 0U && parameters.axes[index] == parameters.axes[0])) {
            return false;
        }
    }
    if ((parameters.valid & DIEP_ENVELOPE) != 0U && parameters.envelope.has_value() &&
        !valid_envelope(*parameters.envelope)) {
        return false;
    }
    return valid_force(parameters);
}

} // namespace inputnext::dinput::force_feedback
