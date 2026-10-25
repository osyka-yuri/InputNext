#pragma once

#include <inputnext/device_metadata.hpp>

namespace inputnext::gameinput_detail {

[[nodiscard]] inline bool is_dualsense_usb_eligible(const metadata_snapshot& metadata) noexcept {
    if (metadata.container_id_source == container_id_provenance::conflicting ||
        !metadata.model.has_value() ||
        (metadata.model.value != metadata_device_model::dualsense &&
            metadata.model.value != metadata_device_model::dualsense_edge) ||
        !metadata.transport.has_value() ||
        metadata.transport.value.value != physical_transport::usb) {
        return false;
    }

    return metadata.transport.value.provenance == transport_provenance::native_bus_type ||
           metadata.transport.value.provenance == transport_provenance::native_enumerator;
}

} // namespace inputnext::gameinput_detail
