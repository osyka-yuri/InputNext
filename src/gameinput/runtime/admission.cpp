#include "state.hpp"

#include "gameinput/core/publication_source.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/error.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/source_lifetime.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace inputnext {

void gameinput_runtime::implementation::device_event(void* context,
    gameinput_detail::native_device_handle device,
    gameinput_detail::native_device_event event) noexcept {
    auto* self = static_cast<implementation*>(context);
    if (self != nullptr) {
        self->on_device_event(device, event);
    }
}

void gameinput_runtime::implementation::on_device_event(
    gameinput_detail::native_device_handle device,
    gameinput_detail::native_device_event event) noexcept {
    std::scoped_lock lock{table_gate};
    if (state == phase::stopping || state == phase::drained || state == phase::failed) {
        return;
    }

    auto matching = owners.end();
    for (auto iterator = owners.begin(); iterator != owners.end(); ++iterator) {
        if (iterator->state != owner_phase::free && iterator->device == device &&
            !iterator->retire_requested) {
            matching = iterator;
            break;
        }
    }

    if (event == gameinput_detail::native_device_event::disconnected) {
        if (matching != owners.end()) {
            matching->retire_requested = true;
            matching->registration_retry_at.reset();
            matching->poll_at.reset();
            matching->facts_retry_at.reset();
        } else {
            request_rescan_locked();
        }
        return;
    }

    if (matching != owners.end()) {
        if ((event == gameinput_detail::native_device_event::connected ||
                event == gameinput_detail::native_device_event::haptic_info_updated) &&
            matching->state != owner_phase::observed) {
            request_facts_locked(*matching);
        }
        return;
    }

    if (event == gameinput_detail::native_device_event::haptic_info_updated) {
        request_rescan_locked();
        return;
    }

    if (next_owner_generation == 0U || owner_generation_exhausted) {
        owner_generation_exhausted = true;
        request_rescan_locked();
        return;
    }
    const auto free_slot = std::ranges::find_if(owners,
        [](const owner_slot& owner) { return owner.state == owner_phase::free; });
    if (free_slot == owners.end()) {
        request_rescan_locked();
        return;
    }

    native->retain(device);
    free_slot->state = owner_phase::observed;
    free_slot->device = device;
    free_slot->generation = next_owner_generation;
    free_slot->reading_owner_context = reading_owner{
        .runtime = this,
        .slot_index = static_cast<std::size_t>(free_slot - owners.begin()),
        .generation = next_owner_generation,
    };
    free_slot->reading_context = gameinput_detail::native_reading_callback_context{
        .owner = &free_slot->reading_owner_context,
        .deliver = &reading_event,
    };
    free_slot->facts_requested = 1U;
    free_slot->facts_installed = 0U;
    free_slot->retire_requested = false;
    free_slot->registry_retire_started = false;
    free_slot->registry_retired = false;
    free_slot->registration_failed = false;
    free_slot->reading_token = {};
    free_slot->admission_retry_at.reset();
    ++next_owner_generation;
    if (next_owner_generation == 0U) {
        owner_generation_exhausted = true;
    }
}

void gameinput_runtime::implementation::service_admissions(maintenance_time now) {
    for (auto& owner : owners) {
        const auto index = static_cast<std::size_t>(&owner - owners.data());
        gameinput_detail::native_device_handle device{};
        std::uint64_t generation{};
        std::uint64_t facts_request{};
        {
            std::scoped_lock lock{table_gate};
            const auto active_or_admitting =
                static_cast<std::size_t>(std::ranges::count_if(owners, [](const owner_slot& owner) {
                    return owner.state == owner_phase::active ||
                           owner.state == owner_phase::admitting;
                }));
            if (active_or_admitting >= gameinput_live_connection_capacity ||
                owner.state != owner_phase::observed || owner.retire_requested ||
                (owner.admission_retry_at.has_value() && *owner.admission_retry_at > now)) {
                continue;
            }
            owner.state = owner_phase::admitting;
            owner.admission_retry_at.reset();
            device = owner.device;
            generation = owner.generation;
            facts_request = owner.facts_requested;
        }

        process_admission(owner_identity{.slot_index = index, .generation = generation},
            device,
            facts_request,
            now);
    }
}

void gameinput_runtime::implementation::retry_admission(owner_identity identity,
    maintenance_time now) noexcept {
    std::scoped_lock lock{table_gate};
    auto& owner = owners[identity.slot_index];
    if (owner.generation == identity.generation && owner.state == owner_phase::admitting) {
        owner.state = owner_phase::observed;
        owner.admission_retry_at = now + runtime_detail::maintenance_period;
        request_rescan_locked();
    }
}

void gameinput_runtime::implementation::process_admission(owner_identity identity,
    gameinput_detail::native_device_handle device,
    std::uint64_t facts_request,
    maintenance_time now) {
    const auto index = identity.slot_index;
    const auto generation = identity.generation;
    if (!native->connected(device)) {
        mark_retiring(index, generation);
        return;
    }

    facts_attempted[index] = true;
    auto observed_facts = native->facts(device);
    std::unique_ptr<source_lifetime> candidate;
    try {
        candidate = std::make_unique<gameinput_detail::publication_source>();
    } catch (const std::bad_alloc&) {
        retry_admission(identity, now);
        return;
    }

    auto* const source = static_cast<gameinput_detail::publication_source*>(candidate.get());
    auto admission = registry.admit(candidate);
    if (!admission.has_value()) {
        if (admission.error() == error::capacity_exceeded) {
            retry_admission(identity, now);
        } else {
            mark_retiring(index, generation);
        }
        return;
    }

    const auto target = admission->target;
    auto pin_result = registry.capture(target);
    if (!pin_result.has_value()) {
        retire_failed_admission(identity, target, nullptr, nullptr);
        return;
    }

    auto residency = std::move(*pin_result);
    if (source->bind(admission->producer) != gameinput_detail::bind_outcome::bound) {
        retire_failed_admission(identity, target, source, &residency);
        return;
    }

    const auto attached =
        attach_admitted_owner(identity, target, source, admission->reader, residency, now);
    if (attached == attachment_status::stale) {
        retire_failed_admission(identity, target, source, &residency);
        return;
    }
    if (attached == attachment_status::retiring) {
        return;
    }

    if (observed_facts.has_value()) {
        const auto installed = install_catalogue_for_request(identity,
            facts_generation{.value = facts_request},
            std::move(*observed_facts));
        acknowledge_facts(owner_identity{.slot_index = index, .generation = generation},
            facts_generation{.value = facts_request},
            installed);
    } else {
        std::scoped_lock lock{table_gate};
        auto& owner = owners[index];
        if (owner.generation == generation && owner.state == owner_phase::active) {
            owner.facts_retry_at = now + runtime_detail::retry_period;
        }
    }

    try_register_readings(index, generation, now);
}

gameinput_runtime::implementation::attachment_status
gameinput_runtime::implementation::attach_admitted_owner(owner_identity identity,
    connection_target target,
    gameinput_detail::publication_source* source,
    read_binding reader,
    read_pin& residency,
    maintenance_time now) {
    std::scoped_lock lock{table_gate};
    auto& owner = owners[identity.slot_index];
    if (owner.generation != identity.generation || owner.state != owner_phase::admitting) {
        return attachment_status::stale;
    }

    owner.target = target;
    owner.source = source;
    owner.reader = reader;
    owner.residency = std::move(residency);
    owner.state = owner_phase::active;
    owner.registration_failed = true;
    owner.registration_retry_at = now;
    owner.poll_at = now;
    owner.facts_retry_at.reset();
    return owner.retire_requested ? attachment_status::retiring : attachment_status::active;
}

bool gameinput_runtime::implementation::retire_registry_target(connection_target target) {
    const auto retirement = registry.retire(target);
    return retirement.has_value() || retirement.error() == error::not_connected ||
           retirement.error() == error::closed;
}

void gameinput_runtime::implementation::retire_failed_admission(owner_identity identity,
    connection_target target,
    gameinput_detail::publication_source* source,
    read_pin* residency) {
    if (source != nullptr) {
        source->close();
    }

    const bool registry_retired = retire_registry_target(target);
    if (registry_retired) {
        if (residency != nullptr) {
            residency->release();
        }
        const auto reclaimed = registry.drain_retired();
        static_cast<void>(reclaimed);
    }

    std::scoped_lock lock{table_gate};
    auto& owner = owners[identity.slot_index];
    if (owner.generation != identity.generation || owner.state != owner_phase::admitting) {
        return;
    }
    owner.retire_requested = true;
    owner.state = owner_phase::retiring;
    if (registry_retired) {
        owner.target = {};
        owner.source = nullptr;
        owner.registry_retire_started = true;
        owner.registry_retired = true;
    } else {
        owner.target = target;
        owner.source = residency == nullptr ? nullptr : source;
        if (residency != nullptr) {
            owner.residency = std::move(*residency);
        }
        owner.registry_retire_started = false;
        owner.registry_retired = false;
    }
}

} // namespace inputnext
