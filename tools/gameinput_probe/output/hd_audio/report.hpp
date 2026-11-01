#pragma once

#include "gameinput_probe/output/hd_audio/evidence.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace inputnext::gameinput_detail {

void write_hd_audio_progress(std::ostream& output,
    connection_target target,
    const std::optional<audio_detail::audio_target_progress>& audio,
    const std::optional<output::backend_progress>& backend);

void write_hd_audio_step(std::ostream& output,
    std::string_view name,
    connection_target target,
    std::size_t step_number,
    std::size_t step_count,
    output::context_identity context,
    const output::body_playback_receipt& receipt,
    hd_usb_evidence usb_evidence,
    const std::optional<hd_usb_match>& match,
    bool pcm_accepted,
    bool fallback_observed);

} // namespace inputnext::gameinput_detail
