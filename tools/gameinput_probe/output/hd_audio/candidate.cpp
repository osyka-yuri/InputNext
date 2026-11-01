#include "gameinput_probe/output/hd_audio/candidate.hpp"

#include "audio/native_types.hpp"
#include "gameinput/output/usb/dualsense_eligibility.hpp"
#include "gameinput_probe/output/dualsense_usb_candidate_selector.hpp"

#include <inputnext/device_metadata.hpp>

#include <optional>

namespace inputnext::gameinput_detail {
std::optional<hd_audio_binding> qualify_hd_audio_binding(const metadata_snapshot& snapshot,
    const dualsense_usb_candidate& candidate) {
    if (!candidate.target.valid() || candidate.catalogue_generation == 0U ||
        snapshot.target != candidate.target ||
        snapshot.catalogue_generation != candidate.catalogue_generation ||
        !is_dualsense_usb_eligible(snapshot) || snapshot.model.value != candidate.model ||
        !snapshot.native.haptic_endpoint_id.has_value() ||
        !audio_detail::fixed_haptic_endpoint_id::create(snapshot.native.haptic_endpoint_id.value)
            .has_value() ||
        !snapshot.container_id.has_value() || snapshot.container_id.value.is_zero() ||
        snapshot.container_id_source == container_id_provenance::conflicting ||
        snapshot.container_id_source == container_id_provenance::unavailable) {
        return std::nullopt;
    }
    if (snapshot.native.container_id.has_value() &&
        snapshot.native.container_id.value != snapshot.container_id.value) {
        return std::nullopt;
    }
    return hd_audio_binding{.endpoint_id = snapshot.native.haptic_endpoint_id.value,
        .container_id = snapshot.container_id.value,
        .model = candidate.model};
}

} // namespace inputnext::gameinput_detail
