#pragma once

#include "rumble_evidence.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/xinput.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace inputnext {
struct gameinput_stop_report;
}

namespace inputnext::focus {
struct snapshot;
}

namespace inputnext::gameinput_detail {

struct rumble_readiness final {
    std::optional<xinput::assignment> assignment;
    bool configured{};
    bool low_motor{};
    bool high_motor{};
    bool foreground{};
    bool window_foreground{};

    [[nodiscard]] bool supported() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
};

struct rumble_step_report final {
    std::string_view step;
    connection_target target{};
    std::uint64_t assignment_id{};
    std::uint64_t request_revision{};
    std::uint64_t logical_version{};
    std::optional<std::uint64_t> logical_channel_version;
    std::uint16_t left_motor{};
    std::uint16_t right_motor{};
    bool logical_channel_present{};
    std::optional<std::uint32_t> logical_result;
    bool logical_call_made{};
    bool logical_state_confirmed{};
    bool assignment_live{};
    bool foreground{};
    bool low_motor_available{};
    bool high_motor_available{};
    bool capabilities_confirmed{};
    rumble_native_evidence native_evidence{rumble_native_evidence::pending};
    std::optional<rumble_native_match> native_match;
};

struct rumble_session_stop_report;

[[nodiscard]] std::string format_rumble_target(connection_target target);
[[nodiscard]] std::string format_rumble_readiness(const rumble_readiness& state,
    const focus::snapshot& focus_snapshot);
[[nodiscard]] std::string format_rumble_step(const rumble_step_report& report);
[[nodiscard]] std::string format_rumble_cleanup_zero(const rumble_step_report& report,
    bool attempted);
[[nodiscard]] std::string format_rumble_connection_stop(const rumble_session_stop_report& report);
[[nodiscard]] std::string format_rumble_runtime_stop(const gameinput_stop_report& report);

} // namespace inputnext::gameinput_detail
