#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <optional>

namespace inputnext::output::detail {

struct represented_projection final {
    connection_target target{};
    authority_incarnation incarnation{};
    channel_mask channels{};
    std::optional<channel_snapshot<trigger_value>> left_trigger = std::nullopt;
    std::optional<channel_snapshot<trigger_value>> right_trigger = std::nullopt;
    std::optional<channel_snapshot<rumble_value>> rumble = std::nullopt;
    std::optional<channel_snapshot<led_value>> leds = std::nullopt;

    friend bool operator==(const represented_projection&, const represented_projection&) = default;
};

[[nodiscard]] execution_result<represented_projection>
project_representation(const logical_snapshot& snapshot, channel_mask channels) noexcept;

[[nodiscard]] native_output native_values(const represented_projection& projection) noexcept;

} // namespace inputnext::output::detail
