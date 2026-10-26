#pragma once

#include "audio/native_types.hpp"

#include <Windows.h>
#include <mmreg.h>

namespace inputnext::audio_detail::windows {

[[nodiscard]] native_audio_format describe_wave_format(const WAVEFORMATEX& value) noexcept;
[[nodiscard]] native_result<WAVEFORMATEXTENSIBLE> make_wave_format(const native_audio_format& value,
    native_operation operation) noexcept;

} // namespace inputnext::audio_detail::windows
