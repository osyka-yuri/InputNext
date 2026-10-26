#include "audio/native_port.hpp"
#include "audio/native_types.hpp"
#include "detail.hpp"
#include "worker_detail.hpp"

#include "gameinput/output/usb/dualsense_eligibility.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>

namespace inputnext::audio_detail {
using clock_type = std::chrono::steady_clock;

namespace {
struct qualified_native_target final {
    native_resource_key resource{};
    std::uint64_t catalogue_generation{};
    bool supported_target{};
};

[[nodiscard]] std::optional<qualified_native_target> qualify_metadata(
    const metadata_snapshot& metadata,
    const connection_target expected_target,
    const std::optional<std::uint64_t> expected_generation) noexcept {
    if (metadata.target != expected_target || metadata.catalogue_generation == 0U ||
        (expected_generation.has_value() &&
            metadata.catalogue_generation != *expected_generation)) {
        return std::nullopt;
    }

    const bool target_supported = gameinput_detail::is_dualsense_usb_eligible(metadata);
    if (!target_supported) {
        return qualified_native_target{.catalogue_generation = metadata.catalogue_generation,
            .supported_target = false};
    }
    const bool authoritative_container =
        metadata.container_id_source == container_id_provenance::gameinput_native ||
        metadata.container_id_source == container_id_provenance::exact_pnp_interface;
    if (!authoritative_container || !metadata.container_id.has_value() ||
        native_guid_is_zero(metadata.container_id.value) ||
        !metadata.native.haptic_endpoint_id.has_value() ||
        metadata.native.haptic_endpoint_id.value.empty()) {
        return std::nullopt;
    }
    const auto endpoint =
        fixed_haptic_endpoint_id::create(metadata.native.haptic_endpoint_id.value);
    if (!endpoint.has_value()) {
        return std::nullopt;
    }
    return qualified_native_target{.resource = native_resource_key{.endpoint = *endpoint,
                                       .container = metadata.container_id.value},
        .catalogue_generation = metadata.catalogue_generation,
        .supported_target = true};
}

[[nodiscard]] bool exact_canonical_format(const native_audio_format& value) noexcept {
    return value == canonical_audio_format();
}

[[nodiscard]] native_result<void> qualify_format(native_port& port) noexcept {
    const auto format = port.get_mix_format();
    if (!format.has_value()) {
        return std::unexpected{format.error()};
    }
    if (!exact_canonical_format(*format)) {
        return std::unexpected{
            native_error{.operation = native_operation::get_mix_format, .native_code = 0U}};
    }
    const auto support = port.is_format_supported(canonical_audio_format());
    if (!support.has_value()) {
        return std::unexpected{support.error()};
    }
    if (support->support != native_format_support::exact || support->closest_format.has_value()) {
        return std::unexpected{
            native_error{.operation = native_operation::is_format_supported, .native_code = 0U}};
    }
    return {};
}

[[nodiscard]] native_result<void> qualify_endpoint(native_port& port,
    const native_resource_key& expected) noexcept {
    const auto endpoint = port.revalidate_exact_endpoint(expected.endpoint);
    if (!endpoint.has_value()) {
        return std::unexpected{endpoint.error()};
    }
    if (endpoint->id != expected.endpoint || endpoint->state != native_endpoint_state::active ||
        endpoint->flow != native_endpoint_flow::render ||
        endpoint->container_id != expected.container) {
        return std::unexpected{
            native_error{.operation = native_operation::get_container_id, .native_code = 0U}};
    }
    return {};
}

[[nodiscard]] native_result<identity_refresh> metadata_refresh_failure(audio_slot& slot,
    const metadata_error error,
    const std::int64_t previous_deadline) noexcept {
    slot.last_metadata_error.store(static_cast<std::uint8_t>(error), std::memory_order_relaxed);
    slot.has_metadata_error.store(true, std::memory_order_relaxed);
    const bool transient =
        error == metadata_error::busy || error == metadata_error::resource_exhausted;
    if (transient && !slot.transport_closed.load(std::memory_order_acquire) &&
        time_ticks(clock_type::now()) < previous_deadline) {
        return identity_refresh::retry_metadata;
    }
    return std::unexpected{native_error{}};
}
} // namespace

[[nodiscard]] native_result<identity_refresh> qualify_fresh_identity(native_port& port,
    gameinput_runtime& runtime,
    audio_slot& slot,
    const native_resource_key& resource,
    const std::uint64_t expected_generation) {
    const auto query_started = clock_type::now();
    const auto previous_deadline = slot.identity_valid_until_ticks.load(std::memory_order_acquire);
    if (slot.transport_closed.load(std::memory_order_acquire) ||
        time_ticks(query_started) >= previous_deadline) {
        return std::unexpected{native_error{}};
    }
    const auto metadata = runtime.query_metadata(slot.target);
    if (!metadata.has_value()) {
        return metadata_refresh_failure(slot, metadata.error(), previous_deadline);
    }
    const auto qualified = qualify_metadata(*metadata, slot.target, expected_generation);
    if (!qualified.has_value() || !qualified->supported_target || qualified->resource != resource) {
        return std::unexpected{
            native_error{.operation = native_operation::none, .native_code = 0U}};
    }
    if (const auto endpoint = qualify_endpoint(port, resource); !endpoint.has_value()) {
        return std::unexpected{endpoint.error()};
    }
    if (const auto format = qualify_format(port); !format.has_value()) {
        return std::unexpected{format.error()};
    }
    const auto now = clock_type::now();
    if (slot.transport_closed.load(std::memory_order_acquire) ||
        time_ticks(now) >= previous_deadline || now - query_started >= audio_identity_validity) {
        return std::unexpected{
            native_error{.operation = native_operation::none, .native_code = 0U}};
    }
    slot.identity_valid_until_ticks.store(time_ticks(query_started + audio_identity_validity),
        std::memory_order_release);
    return identity_refresh::refreshed;
}

namespace {
[[nodiscard]] bool
prefill_silent(native_port& port, audio_slot& slot, std::uint32_t frames) noexcept;

[[nodiscard]] bool begin_and_identify(gameinput_runtime& runtime,
    native_port& port,
    audio_slot& slot,
    native_worker_state& state) noexcept {
    state.sta_attempted = true;
    const auto begun = port.begin_worker_sta();
    if (!begun.has_value()) {
        fail_worker(slot, true, begun.error());
        return false;
    }
    slot.native_setup_started.store(true, std::memory_order_release);
    const auto query_started = clock_type::now();
    const auto metadata = runtime.query_metadata(slot.target);
    if (!metadata.has_value()) {
        slot.last_metadata_error.store(static_cast<std::uint8_t>(metadata.error()),
            std::memory_order_relaxed);
        slot.has_metadata_error.store(true, std::memory_order_relaxed);
        fail_worker(slot, true);
        return false;
    }
    const auto qualified = qualify_metadata(*metadata, slot.target, std::nullopt);
    if (!qualified.has_value()) {
        fail_worker(slot, true);
        return false;
    }
    if (!qualified->supported_target) {
        slot.retry_after_join.store(true, std::memory_order_release);
        slot.transport_closed.store(true, std::memory_order_release);
        slot.setup.store(setup_phase::rejected, std::memory_order_release);
        wake_control(slot);
        return false;
    }
    if (slot.transport_closed.load(std::memory_order_acquire) ||
        clock_type::now() - query_started >= audio_identity_validity) {
        fail_worker(slot, true);
        return false;
    }
    state.resource = qualified->resource;
    state.has_resource = true;
    state.generation = qualified->catalogue_generation;
    slot.candidate_key = state.resource;
    slot.identity_valid_until_ticks.store(time_ticks(query_started + audio_identity_validity),
        std::memory_order_release);
    slot.setup.store(setup_phase::candidate_ready, std::memory_order_release);
    wake_control(slot);
    return true;
}

[[nodiscard]] bool wait_for_reservation(native_port& port, audio_slot& slot) noexcept {
    while (!slot.transport_closed.load(std::memory_order_acquire)) {
        const auto phase = slot.setup.load(std::memory_order_acquire);
        if (phase == setup_phase::granted) {
            return true;
        }
        if (phase == setup_phase::rejected) {
            return false;
        }
        const auto activity = port.wait_for_activity(audio_control_reconcile_interval);
        if (activity.outcome == native_wait_outcome::failed) {
            fail_worker(slot, true, activity.error);
            return false;
        }
        if (activity.outcome == native_wait_outcome::stop_requested) {
            slot.transport_closed.store(true, std::memory_order_release);
            wake_control(slot);
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool open_verified_endpoint(native_port& port,
    audio_slot& slot,
    const native_resource_key& resource) noexcept {
    const auto endpoint = port.open_exact_endpoint(resource.endpoint);
    if (!endpoint.has_value()) {
        fail_worker(slot, true, endpoint.error());
        return false;
    }
    if (endpoint->id == resource.endpoint && endpoint->state == native_endpoint_state::active &&
        endpoint->flow == native_endpoint_flow::render &&
        endpoint->container_id == resource.container) {
        return true;
    }
    fail_worker(slot,
        true,
        native_error{.operation = native_operation::get_container_id, .native_code = 0U});
    return false;
}

[[nodiscard]] bool
initialize_native_stream(native_port& port, audio_slot& slot, native_worker_state& state) noexcept {
    const auto activated = port.activate_audio_client();
    if (!activated.has_value()) {
        fail_worker(slot, true, activated.error());
        return false;
    }
    const auto format = qualify_format(port);
    if (!format.has_value()) {
        fail_worker(slot, true, format.error());
        return false;
    }
    const auto initialized = port.initialize_event_driven_stream(canonical_audio_format());
    if (!initialized.has_value()) {
        fail_worker(slot, true, initialized.error());
        return false;
    }
    const auto service = port.acquire_render_service();
    if (!service.has_value()) {
        fail_worker(slot, true, service.error());
        return false;
    }
    const auto size = port.get_buffer_size();
    if (!size.has_value()) {
        fail_worker(slot, true, size.error());
        return false;
    }
    if (*size == 0U || *size > audio_max_ring_frames) {
        fail_worker(slot,
            true,
            native_error{.operation = native_operation::get_buffer_size, .native_code = 0U});
        return false;
    }
    state.buffer_size = *size;
    if (!prefill_silent(port, slot, state.buffer_size)) {
        fail_worker(slot, true);
        return false;
    }
    const auto mmcss = port.register_mmcss_audio();
    slot.mmcss_registered.store(mmcss.registered, std::memory_order_relaxed);
    if (!mmcss.registered) {
        publish_native_error(slot,
            native_error{.operation = native_operation::register_mmcss,
                .native_code = mmcss.native_code});
    }
    const auto started = port.start_stream();
    if (!started.has_value()) {
        fail_worker(slot, true, started.error());
        return false;
    }
    slot.native_stream_started.store(true, std::memory_order_release);
    return true;
}

[[nodiscard]] bool wait_for_identity_retry(native_port& port, audio_slot& slot) noexcept {
    const auto deadline = clock_type::now() + audio_identity_retry_interval;
    while (!slot.transport_closed.load(std::memory_order_acquire)) {
        const auto now = clock_type::now();
        if (now >= deadline) {
            return true;
        }
        const auto activity =
            port.wait_for_activity(std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        if (activity.outcome == native_wait_outcome::failed) {
            fail_worker(slot, true, activity.error);
            return false;
        }
        if (activity.outcome == native_wait_outcome::stop_requested) {
            slot.transport_closed.store(true, std::memory_order_release);
            wake_control(slot);
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool requalify_after_start(gameinput_runtime& runtime,
    native_port& port,
    audio_slot& slot,
    const native_resource_key& resource,
    const std::uint64_t generation) noexcept {
    for (;;) {
        const auto identity = qualify_fresh_identity(port, runtime, slot, resource, generation);
        if (!identity.has_value()) {
            fail_worker(slot, true, identity.error());
            return false;
        }
        if (*identity == identity_refresh::refreshed) {
            slot.setup.store(setup_phase::prepared, std::memory_order_release);
            wake_control(slot);
            return true;
        }
        if (!wait_for_identity_retry(port, slot)) {
            return false;
        }
    }
}
} // namespace

[[nodiscard]] bool prepare_native_worker(gameinput_runtime& runtime,
    native_port& port,
    audio_slot& slot,
    native_worker_state& state) noexcept {
    if (!begin_and_identify(runtime, port, slot, state) || !wait_for_reservation(port, slot)) {
        return false;
    }
    if (!state.has_resource) {
        fail_worker(slot, true);
        return false;
    }
    const auto resource = state.resource;
    if (!open_verified_endpoint(port, slot, resource) ||
        !initialize_native_stream(port, slot, state)) {
        return false;
    }
    return requalify_after_start(runtime, port, slot, resource, state.generation);
}

namespace {
[[nodiscard]] bool
prefill_silent(native_port& port, audio_slot& slot, const std::uint32_t frames) noexcept {
    if (frames == 0U || frames > audio_max_ring_frames) {
        return false;
    }
    const auto acquired = port.acquire_render_buffer(frames);
    if (!acquired.has_value()) {
        record_error(slot, acquired.error());
        return false;
    }
    const auto released = port.release_render_buffer(native_buffer_disposition::silent);
    if (!released.has_value()) {
        record_error(slot, released.error());
        return false;
    }
    slot.accepted_silent_buffers.add(1U);
    return true;
}
} // namespace
} // namespace inputnext::audio_detail
