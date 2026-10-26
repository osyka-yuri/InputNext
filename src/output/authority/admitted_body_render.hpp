#pragma once

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

namespace inputnext::output::detail {

struct admitted_body_render final {
    body_route_ack route;
    body_playback_identity playback;
    body_render_permit permit;
};

} // namespace inputnext::output::detail
