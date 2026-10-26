#pragma once

#include "admitted_body_render.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace inputnext::output::detail {

enum class completion_disposition : std::uint8_t {
    stale,
    current,
    fallback_committed,
};

class authority_execution_access final {
public:
    [[nodiscard]] static execution_result<execution_owner> attach(authority& owner);
    [[nodiscard]] static execution_result<void> detach(authority& owner,
        execution_owner expected_owner);
    [[nodiscard]] static execution_result<void>
    check_target(authority& owner, execution_owner expected_owner, connection_target target);
    [[nodiscard]] static execution_result<bool> qualifies(authority& owner,
        execution_owner expected_owner,
        const represented_projection& expected);
    [[nodiscard]] static execution_result<bool> qualifies_route(authority& owner,
        execution_owner expected_owner,
        const body_route_intent& expected);
    [[nodiscard]] static execution_result<completion_disposition> complete_route(authority& owner,
        execution_owner expected_owner,
        const body_route_intent& expected,
        native_status status);
    [[nodiscard]] static execution_result<std::optional<body_render_permit>> admit_body_render(
        authority& owner,
        execution_owner expected_owner,
        const body_route_ack& acknowledgement,
        body_playback_identity expected_playback,
        std::atomic<bool>* engine_busy,
        std::size_t requested_frames) noexcept;
    [[nodiscard]] static execution_result<std::optional<admitted_body_render>>
    admit_current_body_render(authority& owner,
        execution_owner expected_owner,
        backend_handle expected_backend,
        const body_route_ack& acknowledgement,
        audio_instance expected_audio,
        std::atomic<bool>* engine_busy,
        std::size_t requested_frames) noexcept;
    static void retire_body_backend(authority& owner, backend_handle backend) noexcept;
};

} // namespace inputnext::output::detail
