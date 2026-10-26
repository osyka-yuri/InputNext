#pragma once

#include "audio/native_types.hpp"

struct IMMDevice;

namespace inputnext::audio_detail::windows {

// Borrows the held device; temporary COM interfaces are released on the calling worker.
[[nodiscard]] native_result<native_endpoint_snapshot> inspect_endpoint(IMMDevice& device) noexcept;

} // namespace inputnext::audio_detail::windows
