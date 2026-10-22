#include "rumble.hpp"

#include "output/authority/state.hpp"
#include "output/composition/composition.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/error.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace inputnext::output::detail {
namespace {

constexpr internal_priority xinput_priority = internal_priority::game_semantic;
constexpr ttl_permission xinput_ttl = ttl_permission::finite_or_indefinite;
constexpr foreground_policy xinput_foreground = foreground_policy::foreground_bound;

[[nodiscard]] internal_context_options options_for(const xinput_service_id service) noexcept {
    return internal_context_options{.owner = internal_owner_key{service.id.value},
        .priority = xinput_priority,
        .ttl = xinput_ttl,
        .foreground = xinput_foreground};
}

[[nodiscard]] error map_registry_error(const inputnext::error value) noexcept {
    switch (value) {
    case inputnext::error::not_connected:
        return error::not_connected;
    case inputnext::error::capacity_exceeded:
    case inputnext::error::identity_exhausted:
        return error::resources;
    case inputnext::error::invalid_argument:
    case inputnext::error::no_data:
    case inputnext::error::revision_exhausted:
    case inputnext::error::closed:
        return error::not_connected;
    }
    return error::resources;
}

[[nodiscard]] bool valid_key(const xinput_service_id owner, const xinput_rumble_key& key) noexcept {
    return key.service == owner && owner.authority.valid() && owner.id.valid() &&
           key.occupant.user_index < 4U && key.occupant.id != 0U && key.occupant.target.valid() &&
           key.enable_generation != 0U && key.epoch.observer_incarnation.valid() &&
           key.epoch.process.pid != 0U && key.epoch.process.creation_time_100ns != 0U &&
           key.epoch.revision != 0U;
}

} // namespace

xinput_rumble::xinput_rumble(authority& owner, xinput_service_id service) noexcept
    : owner_(owner),
      service_(service) {
}

result<std::unique_ptr<xinput_rumble>> xinput_rumble::create(authority& owner) {
    auto& state = *owner.implementation_;
    std::scoped_lock lock{state.gate};
    const auto id = state.take_context_id();
    if (!id.has_value()) {
        return std::unexpected{error::identity_exhausted};
    }
    const auto service = xinput_service_id{.authority = state.incarnation, .id = context_id{*id}};
    auto* bridge = new (std::nothrow) xinput_rumble{owner, service};
    if (bridge == nullptr) {
        return std::unexpected{error::resources};
    }
    return std::unique_ptr<xinput_rumble>{bridge};
}

xinput_rumble_transaction::xinput_rumble_transaction(xinput_rumble& owner)
    : owner_(owner),
      authority_lock_(owner.owner_.implementation_->gate),
      presentation_(owner.owner_.implementation_->focus_observer.lock_presentation()) {
}

bool xinput_rumble::matches_focus(const focus::observer& observer) const noexcept {
    return &owner_.implementation_->focus_observer == &observer;
}

xinput_rumble::transaction xinput_rumble::begin() {
    return xinput_rumble_transaction{*this};
}

focus::observer::presentation_guard& xinput_rumble_transaction::presentation() noexcept {
    return presentation_;
}

bool xinput_rumble_transaction::owns_key(const xinput_rumble_key& key,
    const std::optional<internal_context>& current) const noexcept {
    if (!valid_key(owner_.service_, key) || !current.has_value()) {
        return false;
    }
    auto& state = *owner_.owner_.implementation_;
    const auto* const entry = state.find_open(current->identity);
    return entry != nullptr && entry->xinput_rumble.has_value() && *entry->xinput_rumble == key;
}

bool xinput_rumble_transaction::target_is_qualified(const xinput_rumble_key& key) const noexcept {
    if (!valid_key(owner_.service_, key) || key.occupant.user_index >= target_pins.size()) {
        return false;
    }
    const auto& target_pin = target_pins[key.occupant.user_index];
    if (!target_pin.has_value()) {
        return false;
    }
    return target_pin->target() == key.occupant.target &&
           presentation_.matches_foreground_epoch(key.epoch);
}

result<void> xinput_rumble_transaction::qualify(const xinput_rumble_key& key) {
    if (!valid_key(owner_.service_, key) || key.occupant.user_index >= target_pins.size()) {
        return std::unexpected{error::invalid_argument};
    }
    auto& target_pin = target_pins[key.occupant.user_index];
    if (target_pin.has_value()) {
        if (target_pin->target() == key.occupant.target &&
            presentation_.matches_foreground_epoch(key.epoch)) {
            return {};
        }
        return std::unexpected{error::not_connected};
    }
    if (!presentation_.matches_foreground_epoch(key.epoch)) {
        return std::unexpected{error::revoked};
    }

    auto captured = owner_.owner_.implementation_->capture(key.occupant.target);
    if (!captured.has_value()) {
        return std::unexpected{map_registry_error(captured.error())};
    }
    target_pin.emplace(std::move(*captured));
    if (target_pin->target() != key.occupant.target) {
        return std::unexpected{error::not_connected};
    }
    if (!presentation_.matches_foreground_epoch(key.epoch)) {
        return std::unexpected{error::revoked};
    }
    return {};
}

result<void> xinput_rumble_transaction::preflight_existing(const xinput_rumble_key& key,
    const internal_context& current,
    const std::uint64_t sequence) const noexcept {
    auto& state = *owner_.owner_.implementation_;
    const auto identity = current.identity;
    if (!state.valid_context_identity(identity, owner_class::trusted_internal) ||
        identity.internal_owner != internal_owner_key{owner_.service_.id.value}) {
        return std::unexpected{error::stale_context};
    }
    const auto* const entry = state.find_open(identity);
    if (entry == nullptr || !entry->xinput_rumble.has_value()) {
        return std::unexpected{error::stale_context};
    }
    if (*entry->xinput_rumble != key || entry->identity.target != key.occupant.target) {
        return std::unexpected{error::stale_context};
    }
    if (sequence <= entry->accepted_sequence) {
        return std::unexpected{error::sequence_rejected};
    }
    if (entry->channels[static_cast<std::size_t>(channel::rumble)].revision ==
        std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected{error::revision_exhausted};
    }
    if (!state.ordinary_headroom(state.reservation_count())) {
        return std::unexpected{error::logical_version_exhausted};
    }
    return {};
}

result<std::uint64_t> xinput_rumble_transaction::reserve_new_context_id() noexcept {
    auto& state = *owner_.owner_.implementation_;
    if (state.find_free(owner_class::trusted_internal) == nullptr) {
        return std::unexpected{error::capacity_exceeded};
    }
    if (state.next_context_id == 0U) {
        return std::unexpected{error::identity_exhausted};
    }
    if (!state.ordinary_headroom(state.reservation_count() + 2U)) {
        return std::unexpected{error::logical_version_exhausted};
    }
    return state.take_context_id();
}

result<std::uint64_t> xinput_rumble_transaction::submit(const xinput_rumble_key& key,
    std::optional<internal_context>& current,
    rumble_value value,
    std::uint64_t sequence) {
    auto& state = *owner_.owner_.implementation_;
    if (!valid_key(owner_.service_, key) || !std::isfinite(value.low_frequency) ||
        !std::isfinite(value.high_frequency) || value.low_frequency < 0.0F ||
        value.low_frequency > 1.0F || value.high_frequency < 0.0F || value.high_frequency > 1.0F ||
        sequence == 0U) {
        return std::unexpected{error::invalid_argument};
    }

    const auto entry_time = state.now();
    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(key.occupant.target, evaluation_time));

    auto* entry = static_cast<authority::implementation::record*>(nullptr);
    const bool needs_context = !current.has_value();
    std::uint64_t reserved_id{};
    if (needs_context) {
        const auto reserved = reserve_new_context_id();
        if (!reserved.has_value()) {
            return std::unexpected{reserved.error()};
        }
        reserved_id = *reserved;
    } else {
        const auto ready = preflight_existing(key, *current, sequence);
        if (!ready.has_value()) {
            return std::unexpected{ready.error()};
        }
    }

    const auto qualified = qualify(key);
    if (!qualified.has_value()) {
        return std::unexpected{qualified.error()};
    }
    if (!target_is_qualified(key)) {
        return std::unexpected{error::revoked};
    }

    if (needs_context) {
        auto* const free = state.find_free(owner_class::trusted_internal);
        if (free == nullptr) {
            return std::unexpected{error::capacity_exceeded};
        }
        const auto options = options_for(owner_.service_);
        free->state = authority::implementation::phase::open;
        free->identity = context_identity{.target = key.occupant.target,
            .incarnation = state.incarnation,
            .id = context_id{reserved_id},
            .kind = owner_class::trusted_internal,
            .internal_owner = options.owner};
        free->priority = options.priority;
        free->ttl = options.ttl;
        free->foreground = options.foreground;
        free->opening_epoch = key.epoch;
        free->xinput_rumble = key;
        ++state.logical_version;
        entry = free;
        current = internal_context{entry->identity};
    } else {
        entry = state.find_open(current->identity);
        if (entry == nullptr) {
            return std::unexpected{error::stale_context};
        }
    }

    const auto commit_order = state.logical_version + 1U;
    auto& channel_state = entry->channels[static_cast<std::size_t>(channel::rumble)];
    entry->accepted_values.rumble = value;
    entry->accepted_sequence = sequence;
    ++channel_state.revision;
    channel_state.accepted_sequence = sequence;
    channel_state.accepted_commit_order = commit_order;
    channel_state.active = true;
    channel_state.expires_at.reset();
    state.logical_version = commit_order;
    return commit_order;
}

void xinput_rumble_transaction::revoke_and_ack(std::optional<internal_context>& current) noexcept {
    if (!current.has_value()) {
        return;
    }

    auto& state = *owner_.owner_.implementation_;
    const auto identity = current->identity;
    if (!state.valid_context_identity(identity, owner_class::trusted_internal) ||
        identity.internal_owner != internal_owner_key{owner_.service_.id.value}) {
        current.reset();
        return;
    }

    auto* entry = state.find_open(identity);
    if (entry == nullptr) {
        entry = state.find_retained(identity);
    }
    if (entry == nullptr || !entry->xinput_rumble.has_value() ||
        entry->xinput_rumble->service != owner_.service_) {
        current.reset();
        return;
    }
    if (entry->state == authority::implementation::phase::open &&
        !state.close_record(*entry, close_reason::explicit_close)) {
        return;
    }
    if (entry->receipt.has_value()) {
        entry->reset();
        current.reset();
    }
}

result<std::unique_ptr<xinput_rumble>> composition::make_xinput_rumble() & noexcept {
    try {
        return xinput_rumble::create(owner_);
    } catch (...) {
        return std::unexpected{error::resources};
    }
}

} // namespace inputnext::output::detail
