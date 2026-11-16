#pragma once

#include "parameters.hpp"

#include <bit>
#include <cstddef>
#include <optional>

namespace inputnext::dinput::force_feedback::detail {

inline constexpr DWORD coordinate_mask = DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL;
inline constexpr DWORD selector_mask = DIEFF_OBJECTIDS | DIEFF_OBJECTOFFSETS;
inline constexpr DWORD effect_flag_mask = coordinate_mask | selector_mask;
inline constexpr DWORD supported_parameter_mask = DIEP_DURATION | DIEP_SAMPLEPERIOD | DIEP_GAIN |
                                                  DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL |
                                                  DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE |
                                                  DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
inline constexpr DWORD full_effect_size = static_cast<DWORD>(sizeof(DIEFFECT));
inline constexpr DWORD legacy_effect_size = static_cast<DWORD>(sizeof(DIEFFECT_DX5));
enum class selector_mode : BYTE { object_ids, object_offsets };

[[nodiscard]] const effect_descriptor* registered_effect(
    const effect_descriptor& candidate) noexcept;

[[nodiscard]] inline bool supported_size(const DWORD size) noexcept {
    return size == full_effect_size || size == legacy_effect_size;
}

[[nodiscard]] inline bool has_start_delay(const DWORD size) noexcept {
    return size >= static_cast<DWORD>(offsetof(DIEFFECT, dwStartDelay) + sizeof(DWORD));
}

[[nodiscard]] inline HRESULT validate_parameter_flags(const DWORD selected,
    const effect_descriptor& effect) noexcept {
    const auto supported = effect.static_params | effect.dynamic_params;
    if ((selected & ~supported_parameter_mask) != 0U) {
        return DIERR_INVALIDPARAM;
    }
    return (selected & ~supported) == 0U ? DI_OK : DIERR_UNSUPPORTED;
}

[[nodiscard]] inline std::optional<selector_mode> selector_flags(
    const DWORD effect_flags) noexcept {
    const auto flags = effect_flags & selector_mask;
    if (flags == DIEFF_OBJECTIDS) {
        return selector_mode::object_ids;
    }
    if (flags == DIEFF_OBJECTOFFSETS) {
        return selector_mode::object_offsets;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool has_parameter(const DWORD selected, const DWORD parameter) noexcept {
    return (selected & parameter) != 0U;
}

[[nodiscard]] inline bool valid_effect_flags(const DWORD flags) noexcept {
    return (flags & ~effect_flag_mask) == 0U && std::popcount(flags & selector_mask) <= 1;
}

[[nodiscard]] inline bool complete_fields(const DWORD valid, const DWORD required) noexcept {
    return (valid & required) == required;
}

} // namespace inputnext::dinput::force_feedback::detail
