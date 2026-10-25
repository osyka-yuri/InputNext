#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>

namespace inputnext::output::detail {

class composition;

// A scalar capability for publishing the currently attached audio incarnation.
class audio_route_control final {
public:
    [[nodiscard]] result<audio_instance>
    attach(connection_target target, body_route_support support, bool ready = false);
    [[nodiscard]] result<void> set_ready(audio_instance expected, bool ready);
    [[nodiscard]] result<void> detach(audio_instance expected);

private:
    friend class composition;
    explicit audio_route_control(authority& owner) noexcept;

    authority& owner_;
};

} // namespace inputnext::output::detail
