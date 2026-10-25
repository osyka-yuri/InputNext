#include "core_state.hpp"

#include "core.hpp"
#include "device_identity.hpp"
#include "gameinput/output/usb/dualsense_eligibility.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <new>
#include <span>

namespace inputnext::core {
namespace {

using observed_catalogue = device_catalogue;

using ordered_targets = std::array<connection_target, device_capacity>;

[[nodiscard]] bool target_less(const connection_target& left,
    const connection_target& right) noexcept {
    if (left.device.value != right.device.value) {
        return left.device.value < right.device.value;
    }
    return left.connection.value < right.connection.value;
}

[[nodiscard]] bool same_inventory(const connection_inventory& left,
    const connection_inventory& right,
    runtime_id identity) noexcept {
    if (left.size != right.size || left.size > left.entries.size()) {
        return false;
    }

    std::array<connection_target, device_capacity> left_targets{};
    std::array<connection_target, device_capacity> right_targets{};
    for (std::size_t index = 0U; index < left.size; ++index) {
        left_targets[index] = left.entries[index].target;
        right_targets[index] = right.entries[index].target;
    }
    std::ranges::sort(std::span{left_targets}.first(left.size), target_less);
    std::ranges::sort(std::span{right_targets}.first(right.size), target_less);
    for (std::size_t index = 0U; index < left.size; ++index) {
        if (!left_targets[index].valid() || left_targets[index].runtime != identity ||
            left_targets[index] != right_targets[index]) {
            return false;
        }
        if (index > 0U && left_targets[index] == left_targets[index - 1U]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] result<ordered_targets> inventory_targets(const connection_inventory& inventory,
    const runtime_id identity) noexcept {
    if (inventory.size > inventory.entries.size()) {
        return std::unexpected(status::capacity_exceeded);
    }

    ordered_targets targets{};
    for (std::size_t index = 0U; index < inventory.size; ++index) {
        targets[index] = inventory.entries[index].target;
    }
    std::ranges::sort(std::span{targets}.first(inventory.size), target_less);
    for (std::size_t index = 0U; index < inventory.size; ++index) {
        if (!targets[index].valid() || targets[index].runtime != identity ||
            (index > 0U && targets[index] == targets[index - 1U])) {
            return std::unexpected(status::busy);
        }
    }
    return targets;
}

[[nodiscard]] status metadata_failure_status(const metadata_error error) noexcept {
    switch (error) {
    case metadata_error::closed:
        return status::unavailable;
    case metadata_error::not_connected:
    case metadata_error::stale:
    case metadata_error::busy:
        return status::busy;
    case metadata_error::resource_exhausted:
        return status::capacity_exceeded;
    }
    return status::io_failed;
}

[[nodiscard]] std::uint16_t field_value(const metadata_field<std::uint16_t>& field) noexcept {
    return field.has_value() ? field.value : 0U;
}

[[nodiscard]] std::uint32_t output_channels(const metadata_snapshot& metadata) noexcept {
    if (!metadata.model.has_value()) {
        return 0U;
    }

    if (metadata.model.value == metadata_device_model::dualsense ||
        metadata.model.value == metadata_device_model::dualsense_edge) {
        if (!gameinput_detail::is_dualsense_usb_eligible(metadata)) {
            return 0U;
        }
        return left_trigger_channel | right_trigger_channel | rumble_channel | led_channel;
    }

    if (metadata.model.value == metadata_device_model::generic_gamepad &&
        metadata.native.gameinput_capabilities.has_value() &&
        (metadata.native.gameinput_capabilities.value.low_frequency_rumble ||
            metadata.native.gameinput_capabilities.value.high_frequency_rumble)) {
        return rumble_channel;
    }
    return 0U;
}

[[nodiscard]] device_record project_record(const metadata_snapshot& metadata) {
    const auto identity = detail::fingerprint_device(metadata);
    return device_record{.target = metadata.target,
        .output_channels = output_channels(metadata),
        .vendor_id = field_value(metadata.native.vendor_id),
        .product_id = field_value(metadata.native.product_id),
        .revision = field_value(metadata.native.revision),
        .transport = metadata.transport.has_value() ? metadata.transport.value.value
                                                    : physical_transport::unknown,
        .identity = identity.quality,
        .hardware_fingerprint = identity.fingerprint};
}

void mark_duplicate_fingerprints(observed_catalogue& snapshot) noexcept {
    for (std::size_t left = 0U; left < snapshot.count; ++left) {
        auto& first = snapshot.entries[left];
        if (first.identity != identity_quality::platform) {
            continue;
        }
        for (std::size_t right = left + 1U; right < snapshot.count; ++right) {
            auto& second = snapshot.entries[right];
            if (second.identity == identity_quality::platform &&
                first.hardware_fingerprint == second.hardware_fingerprint) {
                first.identity = identity_quality::ambiguous;
                second.identity = identity_quality::ambiguous;
            }
        }
    }
}

[[nodiscard]] result<observed_catalogue> observe_catalogue_once(gameinput_runtime& runtime,
    const runtime_id identity,
    const std::atomic<bool>& admission_closed) {
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    const auto before = runtime.connections();
    auto targets = inventory_targets(before, identity);
    if (!targets.has_value()) {
        return std::unexpected(targets.error());
    }

    observed_catalogue candidate{};
    candidate.count = before.size;
    for (std::size_t index = 0U; index < before.size; ++index) {
        const auto target = targets->at(index);
        auto metadata = runtime.query_metadata(target);
        if (!metadata.has_value()) {
            const auto failure = metadata_failure_status(metadata.error());
            return std::unexpected(failure);
        }
        if (metadata->target != target) {
            return std::unexpected(status::busy);
        }
        candidate.entries[index] = project_record(*metadata);
    }

    const auto after = runtime.connections();
    if (!same_inventory(before, after, identity)) {
        return std::unexpected(status::busy);
    }
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    mark_duplicate_fingerprints(candidate);
    return candidate;
}

} // namespace

result<device_catalogue> service::implementation::publish_catalogue(
    device_catalogue candidate) noexcept {
    const bool changed = !catalogue_published || catalogue.count != candidate.count ||
                         !std::ranges::equal(std::span{catalogue.entries}.first(catalogue.count),
                             std::span{candidate.entries}.first(candidate.count));
    if (changed && catalogue_revision == std::numeric_limits<std::uint64_t>::max()) {
        catalogue_exhausted = true;
        return std::unexpected(status::unavailable);
    }
    if (changed) {
        ++catalogue_revision;
    }

    candidate.revision = catalogue_revision;
    catalogue = candidate;
    catalogue_published = true;
    return catalogue;
}

result<device_catalogue> service::implementation::enumerate() {
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    try {
        std::scoped_lock catalogue_lock{catalogue_gate};
        if (catalogue_exhausted) {
            return std::unexpected(status::unavailable);
        }

        constexpr std::size_t observation_attempts = 3U;
        for (std::size_t attempt = 0U; attempt < observation_attempts; ++attempt) {
            if (admission_closed.load(std::memory_order_acquire)) {
                return std::unexpected(status::unavailable);
            }
            auto candidate = observe_catalogue_once(runtime, identity, admission_closed);
            if (!candidate.has_value()) {
                if (candidate.error() == status::busy) {
                    continue;
                }
                return std::unexpected(candidate.error());
            }
            if (admission_closed.load(std::memory_order_acquire)) {
                return std::unexpected(status::unavailable);
            }
            return publish_catalogue(*candidate);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(status::capacity_exceeded);
    } catch (...) {
        return std::unexpected(status::io_failed);
    }

    return std::unexpected(status::busy);
}

} // namespace inputnext::core
