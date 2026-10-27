#include "implementation.hpp"

#include "backend_observation.hpp"
#include "error_mapping.hpp"
#include "identity.hpp"

#include "audio/audio_service.hpp"
#include "core/core.hpp"
#include "effects/effects.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "gameinput/runtime/output_access.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>

#include <expected>
#include <mutex>
#include <new>

namespace inputnext::effects {
namespace {

[[nodiscard]] bool sony_model(const metadata_snapshot& metadata) noexcept {
    return metadata.model.has_value() &&
           (metadata.model.value == metadata_device_model::dualsense ||
               metadata.model.value == metadata_device_model::dualsense_edge);
}

[[nodiscard]] bool backend_matches(const gameinput_detail::managed_backend_observation& left,
    const gameinput_detail::managed_backend_observation& right) noexcept {
    return left.kind == right.kind &&
           left.execution.progress.backend == right.execution.progress.backend;
}

[[nodiscard]] status select_fresh_target(const service_dependencies& dependencies,
    const core::selector& selection,
    connection_target& target,
    gameinput_detail::output_description& description,
    metadata_snapshot& metadata) {
    const auto selected = dependencies.core.resolve(selection);
    if (!selected.has_value()) {
        return map_core_error(selected.error());
    }
    target = selected.value();
    if (!target.valid() || target.runtime != dependencies.identity) {
        return status::stale_target;
    }

    const auto described = dependencies.output_access.describe(target);
    if (!described.has_value()) {
        return map_gameinput_error(described.error().code);
    }
    description = described.value();
    if (description.target != target) {
        return status::busy;
    }

    const auto queried = dependencies.runtime.query_metadata(target);
    if (!queried.has_value()) {
        return map_metadata_error(queried.error());
    }
    metadata = queried.value();
    if (metadata.target != target ||
        metadata.catalogue_generation != description.catalogue_generation) {
        return status::busy;
    }
    return status::ok;
}

[[nodiscard]] status qualify_sony_usb(const service_dependencies& dependencies,
    const connection_target target,
    const gameinput_detail::output_description& description,
    const metadata_snapshot& metadata,
    const gameinput_detail::managed_backend_observation& observed,
    bool& eligible) {
    eligible = false;
    if (observed.kind != gameinput_detail::output_backend_kind::dualsense_usb ||
        !sony_model(metadata)) {
        return status::ok;
    }
    const auto qualification =
        dependencies.output_access.qualify_dualsense_usb(target, description.catalogue_generation);
    if (!qualification.has_value()) {
        return map_metadata_error(qualification.error());
    }
    eligible = qualification.value();
    return status::ok;
}

struct motor_support final {
    bool low{};
    bool high{};
};

[[nodiscard]] motor_support read_motor_support(const metadata_snapshot& metadata) noexcept {
    const auto& hardware = metadata.capabilities;
    const auto& native = metadata.native.gameinput_capabilities;
    return motor_support{.low = (hardware.has_value() && hardware.value.low_frequency_rumble) ||
                                (native.has_value() && native.value.low_frequency_rumble),
        .high = (hardware.has_value() && hardware.value.high_frequency_rumble) ||
                (native.has_value() && native.value.high_frequency_rumble)};
}

void apply_sony_channels(const bool eligible,
    const metadata_field<hardware_capabilities>& hardware,
    capabilities& result) noexcept {
    auto& channels = result.supported_channels;
    auto& features = result.features;
    if (!eligible || !hardware.has_value()) {
        channels &= ~(output::left_trigger_mask | output::right_trigger_mask | output::leds_mask);
        return;
    }

    if ((channels & output::left_trigger_mask) != 0U && hardware.value.adaptive_left_trigger) {
        features = features | feature_flags::left_trigger;
    } else {
        channels &= ~output::left_trigger_mask;
    }
    if ((channels & output::right_trigger_mask) != 0U && hardware.value.adaptive_right_trigger) {
        features = features | feature_flags::right_trigger;
    } else {
        channels &= ~output::right_trigger_mask;
    }

    const bool has_led =
        hardware.value.lightbar || hardware.value.player_leds || hardware.value.microphone_led;
    if ((channels & output::leds_mask) == 0U || !has_led) {
        channels &= ~output::leds_mask;
        return;
    }
    if (hardware.value.lightbar) {
        features = features | feature_flags::lightbar;
    }
    if (hardware.value.player_leds) {
        features = features | feature_flags::player_leds;
    }
    if (hardware.value.microphone_led) {
        features = features | feature_flags::microphone_led;
    }
}

void apply_motor_features(const output::backend_progress& native,
    const motor_support motors,
    const output::channel_mask channels,
    feature_mask& features) noexcept {
    if ((channels & output::rumble_mask) == 0U) {
        return;
    }
    if (motors.low) {
        features = features | feature_flags::low_rumble;
    }
    if (motors.high) {
        features = features | feature_flags::high_rumble;
    }
    if (native.body_routes == output::body_route_support::classic_only ||
        native.body_routes == output::body_route_support::hd) {
        features = features | feature_flags::body_lease;
    }
}

void set_completion_capability(const gameinput_detail::output_backend_kind kind,
    capabilities& result) noexcept {
    switch (kind) {
    case gameinput_detail::output_backend_kind::none:
        break;
    case gameinput_detail::output_backend_kind::gameinput_rumble:
        result.completion = completion_capabilities::setter_returned_current;
        break;
    case gameinput_detail::output_backend_kind::dualsense_usb:
        result.completion = completion_capabilities::raw_output_accepted_current;
        break;
    }
}

[[nodiscard]] status attach_hd_if_ready(const service_dependencies& dependencies,
    const connection_target target,
    const metadata_snapshot& metadata,
    const gameinput_detail::managed_backend_observation& observed,
    const bool sony_usb_eligible,
    capabilities& result) {
    if (!sony_usb_eligible ||
        observed.kind != gameinput_detail::output_backend_kind::dualsense_usb ||
        !sony_model(metadata)) {
        return status::ok;
    }
    const auto audio = dependencies.audio.progress(target);
    if (!audio.has_value() || audio->phase != audio_detail::audio_target_phase::live ||
        !audio->ready || !audio->backend.has_value() ||
        audio->backend.value() != observed.execution.progress.backend ||
        !audio->audio.has_value() || audio->audio->target != target) {
        return status::ok;
    }

    const auto inspected = dependencies.authority.inspect_audio(audio->audio.value());
    if (!inspected.has_value() || inspected->instance != audio->audio.value() ||
        inspected->support != output::body_route_support::hd || !inspected->ready) {
        return status::ok;
    }

    gameinput_detail::managed_backend_observation rechecked{};
    const auto backend_status = current_backend(dependencies.connections, target, rechecked);
    if (backend_status != status::ok || !backend_matches(observed, rechecked)) {
        return status::busy;
    }
    result.audio_instance_id = audio->audio->id.value;
    result.features = result.features | feature_flags::hd_ready;
    return status::ok;
}

[[nodiscard]] status populate_capabilities(const service_dependencies& dependencies,
    const connection_target target,
    const gameinput_detail::output_description& description,
    const metadata_snapshot& metadata,
    const gameinput_detail::managed_backend_observation& observed,
    capabilities& result,
    bool& sony_usb_eligible) {
    const auto qualified =
        qualify_sony_usb(dependencies, target, description, metadata, observed, sony_usb_eligible);
    if (qualified != status::ok) {
        return qualified;
    }

    const auto& native = observed.execution.progress;
    auto channels = native.represented_channels;
    const auto motors = read_motor_support(metadata);
    if ((channels & output::rumble_mask) == 0U || (!motors.low && !motors.high)) {
        channels &= ~output::rumble_mask;
    }
    result.supported_channels = channels;
    apply_sony_channels(sony_usb_eligible, metadata.capabilities, result);
    apply_motor_features(native, motors, result.supported_channels, result.features);
    set_completion_capability(observed.kind, result);
    return observed.kind == gameinput_detail::output_backend_kind::none ? status::unsupported_target
                                                                        : status::ok;
}

} // namespace

result<resolution> service::resolve(const core::selector& selection) {
    auto& state = *implementation_;
    {
        std::scoped_lock lock{state.gate};
        if (!state.accepting) {
            return std::unexpected{status::unavailable};
        }
    }

    try {
        connection_target target{};
        gameinput_detail::output_description description{};
        metadata_snapshot metadata{};
        const auto selected =
            select_fresh_target(state.dependencies, selection, target, description, metadata);
        if (selected != status::ok) {
            return std::unexpected{selected};
        }

        gameinput_detail::managed_backend_observation observed{};
        const auto backend_status =
            current_backend(state.dependencies.connections, target, observed);
        if (backend_status != status::ok) {
            return std::unexpected{backend_status};
        }

        resolution resolved{.target = target};
        bool sony_usb_eligible{};
        const auto populated = populate_capabilities(state.dependencies,
            target,
            description,
            metadata,
            observed,
            resolved.value,
            sony_usb_eligible);
        if (populated != status::ok) {
            return std::unexpected{populated};
        }

        gameinput_detail::managed_backend_observation rechecked{};
        const auto recheck_status =
            current_backend(state.dependencies.connections, target, rechecked);
        if (recheck_status != status::ok || !backend_matches(observed, rechecked)) {
            return std::unexpected{status::busy};
        }

        const auto audio_status = attach_hd_if_ready(state.dependencies,
            target,
            metadata,
            observed,
            sony_usb_eligible,
            resolved.value);
        if (audio_status != status::ok) {
            return std::unexpected{audio_status};
        }

        const auto revision = state.dependencies.identities.take_capability();
        if (!revision.has_value()) {
            return std::unexpected{revision.error()};
        }
        resolved.value.observation_revision = revision.value();
        return resolved;
    } catch (const std::bad_alloc&) {
        return std::unexpected{status::capacity_exceeded};
    } catch (...) {
        return std::unexpected{status::io_failed};
    }
}

} // namespace inputnext::effects
