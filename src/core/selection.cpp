#include "core_state.hpp"

#include "core.hpp"
#include <inputnext/device_identity.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <span>

namespace inputnext::core {
namespace {

[[nodiscard]] bool target_is_zero(const connection_target& target) noexcept {
    return std::ranges::all_of(target.runtime.bytes,
               [](const std::byte value) { return value == std::byte{}; }) &&
           target.device.value == 0U && target.connection.value == 0U;
}

[[nodiscard]] bool fingerprint_is_zero(const std::array<std::uint8_t, 32U>& fingerprint) noexcept {
    return std::ranges::all_of(fingerprint, [](const std::uint8_t value) { return value == 0U; });
}

} // namespace

result<connection_target> service::implementation::resolve_slot(const selector& selection) const {
    if (selection.slot >= 4U || !target_is_zero(selection.target) ||
        !fingerprint_is_zero(selection.hardware_fingerprint)) {
        return std::unexpected(status::invalid_argument);
    }

    const auto positions = presentation.positions();
    const auto& position = positions[selection.slot];
    if (!position.has_value()) {
        return std::unexpected(status::unavailable);
    }
    return position.value().target;
}

result<connection_target> service::implementation::resolve_exact_target(const selector& selection) {
    if (selection.slot != 0U || !fingerprint_is_zero(selection.hardware_fingerprint) ||
        !selection.target.valid()) {
        return std::unexpected(status::invalid_argument);
    }
    return selection.target;
}

result<connection_target> service::implementation::resolve_fingerprint(const selector& selection) {
    if (selection.slot != 0U || !target_is_zero(selection.target) ||
        fingerprint_is_zero(selection.hardware_fingerprint)) {
        return std::unexpected(status::invalid_argument);
    }

    const auto observed = enumerate();
    if (!observed.has_value()) {
        return std::unexpected(observed.error());
    }

    std::optional<connection_target> match;
    for (const auto& entry : std::span{observed->entries}.first(observed->count)) {
        if (entry.identity == identity_quality::none ||
            entry.hardware_fingerprint != selection.hardware_fingerprint) {
            continue;
        }
        if (entry.identity == identity_quality::ambiguous || match.has_value()) {
            return std::unexpected(status::ambiguous_target);
        }
        match = entry.target;
    }
    if (!match.has_value()) {
        return std::unexpected(status::unavailable);
    }
    return match.value();
}

result<connection_target> service::implementation::resolve(const selector& selection) {
    if (admission_closed.load(std::memory_order_acquire)) {
        return std::unexpected(status::unavailable);
    }

    result<connection_target> selected = std::unexpected(status::invalid_argument);
    switch (selection.kind) {
    case selector_kind::xinput_slot:
        selected = resolve_slot(selection);
        break;
    case selector_kind::exact_target:
        selected = resolve_exact_target(selection);
        break;
    case selector_kind::hardware_fingerprint:
        selected = resolve_fingerprint(selection);
        break;
    default:
        return std::unexpected(status::invalid_argument);
    }
    if (!selected.has_value()) {
        return std::unexpected(selected.error());
    }

    const auto target = *selected;
    if (!target.valid() || target.runtime != identity) {
        return std::unexpected(status::stale_target);
    }

    try {
        const auto current = registry.capture(target);
        return current.has_value()
                   ? result<connection_target>{target}
                   : result<connection_target>{std::unexpected(status::stale_target)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(status::capacity_exceeded);
    } catch (...) {
        return std::unexpected(status::io_failed);
    }
}

} // namespace inputnext::core
