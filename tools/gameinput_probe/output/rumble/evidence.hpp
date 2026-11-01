#pragma once

#include "gameinput_probe/output/rumble/sequence.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

enum class rumble_native_evidence : std::uint8_t {
    not_requested,
    pending,
    accepted_newer_attempt,
    historical_reuse,
    rejected,
};

struct rumble_native_match final {
    output::backend_handle backend{};
    output::native_output output{};
    output::publication_id publication{};
    output::attempt_id attempt{};
    output::native_result result{};

    friend bool operator==(const rumble_native_match&, const rumble_native_match&) = default;
};

struct rumble_native_observation final {
    output::backend_progress progress{};
    bool logical_channel_present{};
    output::rumble_value logical_value{};
};

[[nodiscard]] rumble_native_evidence assess_native_evidence(const rumble_step& step,
    const output::backend_handle& expected_backend,
    std::optional<output::attempt_id> before_attempt,
    const rumble_native_observation& observation,
    const std::optional<rumble_native_match>& previous_match,
    rumble_native_match& matched) noexcept;

[[nodiscard]] bool assess_logical_evidence(const rumble_step& step,
    const std::optional<xinput::rumble_output_status>& before_status,
    const std::optional<xinput::rumble_output_status>& after_status,
    std::optional<std::uint32_t> set_state_result,
    bool projection_available,
    bool logical_channel_present,
    output::rumble_value logical_value,
    std::optional<std::uint64_t> logical_channel_version) noexcept;

} // namespace inputnext::gameinput_detail
