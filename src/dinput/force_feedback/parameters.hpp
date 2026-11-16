#pragma once

#include "dinput/format.hpp"
#include "dinput/native_types.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <variant>

namespace inputnext::dinput::force_feedback {

enum class waveform : BYTE {
    constant,
    ramp,
    square,
    sine,
    triangle,
    sawtooth_up,
    sawtooth_down,
};

struct effect_descriptor final {
    const GUID* id{};
    waveform wave{};
    const char* name{};
    const wchar_t* wide_name{};
    DWORD type{};
    DWORD static_params{};
    DWORD dynamic_params{};
};

[[nodiscard]] std::span<const effect_descriptor> effects() noexcept;
[[nodiscard]] const effect_descriptor* find_effect(REFGUID id) noexcept;
[[nodiscard]] bool actuator(std::size_t profile_object_index) noexcept;

// Values are copied from DirectInput; the scheduler does not retain COM caller memory.
struct effect_parameters final {
    DWORD valid{DIEP_GAIN | DIEP_SAMPLEPERIOD | DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL |
                DIEP_ENVELOPE | DIEP_STARTDELAY};
    DWORD duration{};
    DWORD sample_period{};
    DWORD gain{DI_FFNOMINALMAX};
    DWORD start_delay{};
    DWORD trigger_button{DIEB_NOTRIGGER};
    DWORD trigger_repeat_interval{INFINITE};
    std::optional<DIENVELOPE> envelope;
    std::variant<DICONSTANTFORCE, DIRAMPFORCE, DIPERIODIC> force{DICONSTANTFORCE{}};
    DWORD axis_count{};
    DWORD direction_count{};
    DWORD coordinate_flags{DIEFF_CARTESIAN};
    std::array<std::size_t, 2U> axes{};
    std::array<LONG, 2U> directions{};
};

[[nodiscard]] HRESULT set_parameters(effect_parameters& destination,
    const effect_descriptor& candidate,
    const DIEFFECT* source,
    DWORD selected,
    const compiled_format* format) noexcept;
[[nodiscard]] HRESULT get_parameters(const effect_parameters& source,
    const effect_descriptor& candidate,
    DIEFFECT* destination,
    DWORD selected,
    const compiled_format* format) noexcept;
[[nodiscard]] bool complete(const effect_parameters& parameters) noexcept;

} // namespace inputnext::dinput::force_feedback
