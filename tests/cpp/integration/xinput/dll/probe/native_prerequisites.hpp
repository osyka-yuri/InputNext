#pragma once

#include <cstdint>

namespace inputnext::xinput_dll_tests {

enum class native_prerequisite_state : std::uint8_t {
    foreground_missing,
    production_bundle_mismatch,
    controller_disconnected,
    ready,
};

struct native_prerequisite_observation {
    bool foreground{};
    bool production_bundle_matches{};
    bool controller_connected{};
};

[[nodiscard]] constexpr native_prerequisite_state decide_native_prerequisites(
    const native_prerequisite_observation& observation) noexcept {
    if (!observation.foreground) {
        return native_prerequisite_state::foreground_missing;
    }
    if (!observation.production_bundle_matches) {
        return native_prerequisite_state::production_bundle_mismatch;
    }
    if (!observation.controller_connected) {
        return native_prerequisite_state::controller_disconnected;
    }
    return native_prerequisite_state::ready;
}

} // namespace inputnext::xinput_dll_tests
