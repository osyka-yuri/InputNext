#pragma once

#include "gameinput/input_profile.hpp"
#include "gameinput/input_qualification.hpp"
#include <inputnext/device_metadata.hpp>

#include <string_view>

#include <GameInput.h>
#include <Windows.h>

namespace inputnext::gameinput_detail {

[[nodiscard]] metadata_field<input_version> loaded_input_runtime_version(HMODULE module);

// Runs on the maintenance thread. No OS query or descriptor allocation is allowed in a callback.
[[nodiscard]] native_input_profile describe_windows_input(GameInput::v4::IGameInputDevice& device,
    const metadata_field<input_version>& runtime_version,
    std::string_view pnp_path);

} // namespace inputnext::gameinput_detail
