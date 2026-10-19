#include "state.hpp"

#include "gameinput/core/publication_source.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/reading_parts.hpp"
#include <inputnext/gameinput_runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace inputnext {

void gameinput_runtime::implementation::reading_event(void* context,
    const gameinput_detail::reading_parts& parts) noexcept {
    auto const* owner = static_cast<reading_owner*>(context);
    if (owner != nullptr && owner->runtime != nullptr) {
        owner->runtime->on_reading_event(
            owner_identity{.slot_index = owner->slot_index, .generation = owner->generation},
            parts);
    }
}

bool gameinput_runtime::implementation::polling_required_locked(const owner_slot& owner) {
    if (owner.state != owner_phase::active || owner.retire_requested ||
        owner.reading_registration_inflight || owner.source == nullptr) {
        return false;
    }
    const auto status = owner.source->status();
    return !owner.reading_token.valid() || owner.registration_failed || !status.pad_known;
}

void gameinput_runtime::implementation::on_reading_event(owner_identity identity,
    const gameinput_detail::reading_parts& parts) noexcept {
    std::scoped_lock lock{table_gate};
    if (identity.slot_index >= owners.size()) {
        return;
    }
    auto& owner = owners[identity.slot_index];
    if (owner.state != owner_phase::active || owner.generation != identity.generation ||
        owner.retire_requested || owner.source == nullptr) {
        return;
    }
    (void)owner.source->on_callback(parts);
    if (!polling_required_locked(owner)) {
        owner.poll_at.reset();
    }
}

void gameinput_runtime::implementation::service_reading_drains(maintenance_time now) {
    for (auto& owner : owners) {
        gameinput_detail::native_callback_token token{};
        std::uint64_t generation{};
        {
            std::scoped_lock lock{table_gate};
            if (!owner.reading_token.valid() || owner.reading_registration_inflight ||
                (owner.reading_drain_retry_at.has_value() && *owner.reading_drain_retry_at > now)) {
                continue;
            }
            if (owner.state != owner_phase::retiring && !owner.retire_requested &&
                !owner.registration_failed) {
                continue;
            }
            owner.reading_registration_inflight = true;
            token = owner.reading_token;
            generation = owner.generation;
        }

        const bool drained = native->unregister_callback(token);
        {
            std::scoped_lock lock{table_gate};
            if (owner.generation != generation) {
                continue;
            }
            owner.reading_registration_inflight = false;
            if (drained) {
                owner.reading_token = {};
                owner.reading_drain_retry_at.reset();
                if (owner.state != owner_phase::retiring && !owner.retire_requested) {
                    owner.registration_failed = true;
                    owner.registration_retry_at = now + runtime_detail::retry_period;
                }
            } else {
                owner.reading_drain_retry_at = now + runtime_detail::retry_period;
            }
        }
    }
}

void gameinput_runtime::implementation::service_readings(maintenance_time now) {
    for (std::size_t index = 0; index < owners.size(); ++index) {
        std::uint64_t generation{};
        bool attempt_registration{};
        {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.state != owner_phase::active || owner.retire_requested ||
                owner.source == nullptr || owner.reading_registration_inflight) {
                continue;
            }
            generation = owner.generation;
            attempt_registration =
                !owner.reading_token.valid() &&
                (!owner.registration_retry_at.has_value() || *owner.registration_retry_at <= now) &&
                (!owner.reading_drain_retry_at.has_value());
        }

        if (attempt_registration) {
            try_register_readings(index, generation, now);
        }
        bool poll{};
        {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.generation == generation) {
                if (polling_required_locked(owner)) {
                    poll = !owner.poll_at.has_value() || *owner.poll_at <= now;
                } else {
                    owner.poll_at.reset();
                }
            }
        }
        if (poll) {
            poll_current(index, generation, now);
        }
    }
}

void gameinput_runtime::implementation::try_register_readings(std::size_t index,
    std::uint64_t generation,
    maintenance_time now) {
    gameinput_detail::native_device_handle device{};
    gameinput_detail::native_reading_callback_context* context{};
    gameinput_detail::publication_source* source{};
    gameinput_detail::seed_ticket seed{};
    {
        std::scoped_lock lock{table_gate};
        if (index >= owners.size()) {
            return;
        }
        auto& owner = owners[index];
        if (owner.state != owner_phase::active || owner.generation != generation ||
            owner.retire_requested || owner.reading_token.valid() ||
            owner.reading_registration_inflight || owner.source == nullptr ||
            (owner.reading_drain_retry_at.has_value() && *owner.reading_drain_retry_at > now) ||
            (owner.registration_retry_at.has_value() && *owner.registration_retry_at > now)) {
            return;
        }
        owner.reading_registration_inflight = true;
        owner.registration_retry_at.reset();
        device = owner.device;
        context = &owner.reading_context;
        source = owner.source;
        seed = source->before_seed();
    }

    auto registered = native->register_reading_callback(device, *context);
    gameinput_detail::native_callback_token token{};
    const bool success = registered.has_value() && registered->valid();
    if (success) {
        token = *registered;
    } else if (!registered.has_value()) {
        token = registered.error().partial_token;
    }

    bool activate_history{};
    {
        std::scoped_lock lock{table_gate};
        auto& owner = owners[index];
        if (owner.generation != generation) {
            if (token.valid()) {
                // The owner cannot be reused while registration is in flight.
                owner.reading_registration_inflight = false;
            }
            return;
        }
        owner.reading_registration_inflight = false;
        if (token.valid()) {
            owner.reading_token = token;
        }
        if (success && owner.state == owner_phase::active && !owner.retire_requested) {
            owner.registration_failed = false;
            owner.registration_retry_at.reset();
            owner.reading_drain_retry_at.reset();
            owner.poll_at = now;
            activate_history = true;
        } else if (!owner.retire_requested && owner.state == owner_phase::active) {
            owner.registration_failed = true;
            owner.registration_retry_at = now + runtime_detail::retry_period;
            owner.poll_at = now;
            if (token.valid()) {
                owner.reading_drain_retry_at = now + runtime_detail::retry_period;
            }
        }
    }

    if (source != nullptr) {
        source->history().set_callback_coverage(activate_history);
    }
    if (activate_history) {
        seed_current(index, generation, seed, now);
    } else if (!registered.has_value() &&
               runtime_detail::is_device_disconnected(registered.error())) {
        mark_retiring(index, generation);
    }
}

void gameinput_runtime::implementation::seed_current(std::size_t index,
    std::uint64_t generation,
    gameinput_detail::seed_ticket seed,
    maintenance_time now) {
    gameinput_detail::native_device_handle device{};
    {
        std::scoped_lock lock{table_gate};
        if (index >= owners.size() || owners[index].generation != generation ||
            owners[index].state != owner_phase::active || owners[index].retire_requested) {
            return;
        }
        device = owners[index].device;
    }
    fetch_seed_lane(index,
        generation,
        device,
        gameinput_detail::native_reading_lane::gamepad,
        seed);
    fetch_seed_lane(index,
        generation,
        device,
        gameinput_detail::native_reading_lane::system_button,
        seed);
    {
        std::scoped_lock lock{table_gate};
        if (index < owners.size() && owners[index].generation == generation &&
            owners[index].state == owner_phase::active && !owners[index].retire_requested) {
            auto& owner = owners[index];
            if (polling_required_locked(owner)) {
                owner.poll_at = now + runtime_detail::maintenance_period;
            } else {
                owner.poll_at.reset();
            }
        }
    }
}

void gameinput_runtime::implementation::fetch_seed_lane(std::size_t index,
    std::uint64_t generation,
    gameinput_detail::native_device_handle device,
    gameinput_detail::native_reading_lane lane,
    gameinput_detail::seed_ticket seed) {
    auto current = native->current(device, lane);
    if (!current.has_value()) {
        if (runtime_detail::is_device_disconnected(current.error())) {
            mark_retiring(index, generation);
        }
        return;
    }
    std::scoped_lock lock{table_gate};
    if (index >= owners.size()) {
        return;
    }
    auto const& owner = owners[index];
    if (owner.generation == generation && owner.state == owner_phase::active &&
        !owner.retire_requested && owner.source != nullptr) {
        (void)owner.source->on_seed(seed, *current);
    }
}

void gameinput_runtime::implementation::poll_current(std::size_t index,
    std::uint64_t generation,
    maintenance_time now) {
    gameinput_detail::native_device_handle device{};
    gameinput_detail::publication_source const* source{};
    gameinput_detail::seed_ticket seed{};
    bool poll_guide{};
    {
        std::scoped_lock lock{table_gate};
        auto& owner = owners[index];
        if (owner.generation != generation || owner.state != owner_phase::active ||
            owner.retire_requested || owner.source == nullptr) {
            return;
        }
        device = owner.device;
        source = owner.source;
        seed = source->before_seed();
        poll_guide = owner.registration_failed || !owner.reading_token.valid();
        owner.poll_at = now + runtime_detail::maintenance_period;
    }

    fetch_seed_lane(index,
        generation,
        device,
        gameinput_detail::native_reading_lane::gamepad,
        seed);
    if (poll_guide) {
        fetch_seed_lane(index,
            generation,
            device,
            gameinput_detail::native_reading_lane::system_button,
            seed);
    }
    {
        std::scoped_lock lock{table_gate};
        if (index < owners.size() && owners[index].generation == generation) {
            auto& owner = owners[index];
            if (!polling_required_locked(owner)) {
                owner.poll_at.reset();
            }
        }
    }
}

} // namespace inputnext
