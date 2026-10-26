#pragma once

#include "audio/audio_service.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

enum class hd_usb_evidence : std::uint8_t {
    pending,
    accepted,
    rejected,
};

struct hd_usb_match final {
    output::backend_handle backend{};
    output::body_route_intent intent{};
    output::native_output native_output{};
    output::publication_id publication{};
    output::attempt_id attempt{};
    output::native_result result{};

    friend bool operator==(const hd_usb_match&, const hd_usb_match&) = default;
};

[[nodiscard]] hd_usb_evidence assess_hd_usb_evidence(const output::backend_handle& expected_backend,
    const output::body_snapshot& expected_body,
    const output::native_output& expected_output,
    std::optional<output::attempt_id> before_attempt,
    const output::backend_progress& progress,
    const output::logical_snapshot& logical,
    const std::optional<hd_usb_match>& previous_match,
    hd_usb_match& matched) noexcept;

[[nodiscard]] bool accepted_hd_pcm_progress(const output::backend_handle& backend,
    const output::audio_instance& audio,
    const output::body_route_ack& route,
    const output::body_playback_identity& playback,
    const audio_detail::audio_target_progress& progress,
    std::uint64_t previous_buffers,
    std::uint64_t previous_frames,
    std::uint64_t previous_source_frames,
    std::uint64_t required_source_frames) noexcept;

[[nodiscard]] const char* hd_usb_evidence_name(hd_usb_evidence value) noexcept;

} // namespace inputnext::gameinput_detail
