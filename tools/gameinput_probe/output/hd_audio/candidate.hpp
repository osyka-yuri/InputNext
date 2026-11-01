#pragma once

#include "gameinput_probe/output/dualsense_usb_candidate_selector.hpp"

#include <inputnext/device_metadata.hpp>

#include <optional>
#include <string>

namespace inputnext::gameinput_detail {

struct hd_audio_binding final {
    std::u16string endpoint_id;
    native_guid container_id;
    metadata_device_model model{metadata_device_model::dualsense};

    friend bool operator==(const hd_audio_binding&, const hd_audio_binding&) = default;
};

[[nodiscard]] std::optional<hd_audio_binding> qualify_hd_audio_binding(
    const metadata_snapshot& snapshot,
    const dualsense_usb_candidate& candidate);

} // namespace inputnext::gameinput_detail
