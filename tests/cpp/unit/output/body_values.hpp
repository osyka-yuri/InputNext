#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <cstddef>

namespace inputnext::test::output::body_values {

using inputnext::output::audio_instance;
using inputnext::output::authority_incarnation;
using inputnext::output::body_lease_id;
using inputnext::output::body_lease_identity;
using inputnext::output::context_id;
using inputnext::output::owner_class;

[[nodiscard]] inline connection_target test_target() {
    return connection_target{.runtime = {.bytes = {std::byte{0x21U}}},
        .device = device_key{4U},
        .connection = connection_id{9U}};
}

[[nodiscard]] inline authority_incarnation test_incarnation() {
    authority_incarnation value{};
    value.bytes[0U] = std::byte{0x31U};
    return value;
}

[[nodiscard]] inline body_lease_identity test_identity(audio_instance audio = {}) {
    const auto target = test_target();
    const auto incarnation = test_incarnation();
    return body_lease_identity{
        .context = {.target = target,
            .incarnation = incarnation,
            .id = context_id{7U},
            .kind = owner_class::public_game},
        .lease = body_lease_id{11U},
        .audio = audio,
    };
}

} // namespace inputnext::test::output::body_values
