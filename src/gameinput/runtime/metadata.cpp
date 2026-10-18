#include "state.hpp"

#include "factory.hpp"
#include "gameinput/native/native_gameinput.hpp"
#include "metadata/provider.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace inputnext {
namespace {
constexpr std::size_t metadata_query_capacity = 4U;
}

[[nodiscard]] metadata_result<metadata_snapshot> gameinput_runtime::implementation::query_metadata(
    connection_target target) {
    if (!target.valid()) {
        return std::unexpected(metadata_error::not_connected);
    }

    std::uint64_t slot_generation{};
    std::uint64_t request_generation{};
    {
        std::scoped_lock lock{table_gate};
        if (state == phase::stopping || state == phase::drained || state == phase::failed) {
            return std::unexpected(metadata_error::closed);
        }
        if (state != phase::running) {
            return std::unexpected(metadata_error::not_connected);
        }
        const auto* const owner = find_live_owner(target);
        if (owner == nullptr) {
            return std::unexpected(metadata_error::not_connected);
        }
        if (owner->metadata_fault) {
            return std::unexpected(metadata_error::resource_exhausted);
        }
        if (owner->facts_installed != owner->facts_requested) {
            return std::unexpected(metadata_error::stale);
        }
        if (active_queries >= metadata_query_capacity) {
            return std::unexpected(metadata_error::busy);
        }
        ++active_queries;
        slot_generation = owner->generation;
        request_generation = owner->facts_requested;
    }

    const auto release_query = [this](implementation*) noexcept {
        {
            std::scoped_lock lock{table_gate};
            if (active_queries > 0U) {
                --active_queries;
            }
        }
        query_drained.notify_all();
    };
    const std::unique_ptr<implementation, decltype(release_query)> query_lease{this, release_query};

    auto pin_result = registry.capture(target);
    if (!pin_result.has_value()) {
        return std::unexpected(metadata_error::not_connected);
    }
    const auto pin = std::move(*pin_result);

    metadata_request request{};
    try {
        std::scoped_lock lock{catalogue_gate};
        const auto* entry = find_catalogue(target, slot_generation);
        if (entry == nullptr || entry->request_generation != request_generation) {
            return std::unexpected(metadata_error::stale);
        }
        request = metadata_request{
            .target = entry->target,
            .catalogue_generation = entry->catalogue_generation,
            .facts = entry->facts,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(metadata_error::resource_exhausted);
    }

    metadata_result<metadata_snapshot> queried =
        std::unexpected(metadata_error::resource_exhausted);
    try {
        queried = metadata->query(request);
    } catch (const std::bad_alloc&) {
        return std::unexpected(metadata_error::resource_exhausted);
    }
    if (!queried.has_value()) {
        return std::unexpected(queried.error());
    }

    {
        std::scoped_lock lock{catalogue_gate};
        const auto* entry = find_catalogue(target, slot_generation);
        if (entry == nullptr || entry->request_generation != request_generation ||
            entry->catalogue_generation != request.catalogue_generation) {
            return std::unexpected(metadata_error::stale);
        }
    }
    {
        std::scoped_lock lock{table_gate};
        const auto* owner = find_live_owner(target);
        if (state != phase::running || owner == nullptr || owner->generation != slot_generation ||
            owner->metadata_fault || owner->facts_requested != request_generation ||
            owner->facts_installed != request_generation) {
            return std::unexpected(metadata_error::stale);
        }
    }

    const auto exact_registry_target = registry.capture(target);
    if (!exact_registry_target.has_value()) {
        return std::unexpected(metadata_error::stale);
    }

    queried->target = target;
    queried->catalogue_generation = request.catalogue_generation;
    return queried;
}

[[nodiscard]] gameinput_runtime::implementation::owner_slot*
gameinput_runtime::implementation::find_live_owner(connection_target target) noexcept {
    for (auto& owner : owners) {
        if (owner.target == target && owner.state == owner_phase::active &&
            !owner.retire_requested) {
            return &owner;
        }
    }
    return nullptr;
}

[[nodiscard]] const gameinput_runtime::implementation::owner_slot*
gameinput_runtime::implementation::find_live_owner(connection_target target) const noexcept {
    for (const auto& owner : owners) {
        if (owner.target == target && owner.state == owner_phase::active &&
            !owner.retire_requested) {
            return &owner;
        }
    }
    return nullptr;
}

[[nodiscard]] const gameinput_runtime::implementation::catalogue_entry*
gameinput_runtime::implementation::find_catalogue(connection_target target,
    std::uint64_t slot_generation) const noexcept {
    const auto found =
        std::ranges::find_if(catalogue, [target, slot_generation](const auto& entry) {
            return entry.active && entry.target == target &&
                   entry.slot_generation == slot_generation;
        });
    return found == catalogue.end() ? nullptr : &*found;
}

void gameinput_runtime::implementation::service_facts_refresh(maintenance_time now) {
    for (std::size_t index = 0; index < owners.size(); ++index) {
        if (facts_attempted[index]) {
            continue;
        }
        gameinput_detail::native_device_handle device{};
        connection_target target{};
        std::uint64_t generation{};
        std::uint64_t request_generation{};
        {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.state != owner_phase::active || owner.retire_requested ||
                owner.metadata_fault || owner.facts_requested == owner.facts_installed ||
                (owner.facts_retry_at.has_value() && *owner.facts_retry_at > now)) {
                continue;
            }
            device = owner.device;
            target = owner.target;
            generation = owner.generation;
            request_generation = owner.facts_requested;
            owner.facts_retry_at.reset();
        }

        facts_attempted[index] = true;
        auto facts = native->facts(device);
        if (!facts.has_value()) {
            std::scoped_lock lock{table_gate};
            auto& owner = owners[index];
            if (owner.generation == generation && owner.state == owner_phase::active &&
                !owner.retire_requested) {
                if (runtime_detail::is_device_disconnected(facts.error())) {
                    owner.retire_requested = true;
                } else {
                    owner.facts_retry_at = now + runtime_detail::retry_period;
                }
            }
            continue;
        }

        const auto installed = install_catalogue_for_request(
            owner_identity{.slot_index = index, .generation = generation},
            facts_generation{.value = request_generation},
            std::move(*facts));
        acknowledge_facts(owner_identity{.slot_index = index, .generation = generation},
            facts_generation{.value = request_generation},
            installed);
    }
}

std::optional<std::uint64_t> gameinput_runtime::implementation::install_catalogue_for_request(
    owner_identity identity,
    facts_generation request_generation,
    native_device_facts facts) {
    connection_target target{};
    {
        std::scoped_lock lock{table_gate};
        if (state != phase::running || identity.slot_index >= owners.size()) {
            return std::nullopt;
        }
        const auto& owner = owners[identity.slot_index];
        if (owner.generation != identity.generation || owner.state != owner_phase::active ||
            owner.retire_requested || owner.metadata_fault ||
            owner.facts_requested != request_generation.value) {
            return std::nullopt;
        }
        target = owner.target;
    }

    // The catalogue write is tagged with this request. A later callback may race after this
    // preinstall check, but acknowledgement and query revalidation reject that stale generation.
    return install_catalogue(target,
        identity.generation,
        request_generation.value,
        std::move(facts));
}

[[nodiscard]] std::optional<std::uint64_t> gameinput_runtime::implementation::install_catalogue(
    connection_target target,
    std::uint64_t slot_generation,
    std::uint64_t request_generation,
    native_device_facts facts) {
    catalogue_entry displaced{};
    std::optional<std::uint64_t> installed;
    {
        std::scoped_lock lock{catalogue_gate};
        auto found = std::ranges::find_if(catalogue, [target, slot_generation](const auto& entry) {
            return entry.active && entry.target == target &&
                   entry.slot_generation == slot_generation;
        });
        if (found == catalogue.end()) {
            found =
                std::ranges::find_if(catalogue, [](const auto& entry) { return !entry.active; });
        }
        if (found != catalogue.end() &&
            catalogue_generation != std::numeric_limits<std::uint64_t>::max()) {
            displaced = std::move(*found);
            ++catalogue_generation;
            *found = catalogue_entry{
                .active = true,
                .target = target,
                .slot_generation = slot_generation,
                .request_generation = request_generation,
                .catalogue_generation = catalogue_generation,
                .facts = std::move(facts),
            };
            installed = catalogue_generation;
        }
    }
    return installed;
}

void gameinput_runtime::implementation::acknowledge_facts(owner_identity identity,
    facts_generation request_generation,
    std::optional<std::uint64_t> installed) {
    std::scoped_lock lock{table_gate};
    auto& owner = owners[identity.slot_index];
    if (owner.generation != identity.generation || owner.state != owner_phase::active ||
        owner.retire_requested) {
        return;
    }
    if (owner.facts_requested != request_generation.value) {
        return;
    }
    if (!installed.has_value()) {
        owner.metadata_fault = true;
        owner.facts_retry_at.reset();
        return;
    }
    owner.facts_installed = request_generation.value;
    owner.facts_retry_at.reset();
}

void gameinput_runtime::implementation::erase_catalogue(connection_target target,
    std::uint64_t slot_generation) noexcept {
    catalogue_entry displaced{};
    {
        std::scoped_lock lock{catalogue_gate};
        const auto found =
            std::ranges::find_if(catalogue, [target, slot_generation](const auto& entry) {
                return entry.active && entry.target == target &&
                       entry.slot_generation == slot_generation;
            });
        if (found != catalogue.end()) {
            displaced = std::move(*found);
            *found = catalogue_entry{};
        }
    }
}

bool gameinput_detail::runtime_factory::seed_facts_generation(gameinput_runtime& runtime,
    connection_target target,
    gameinput_detail::facts_generation_seed seed) {
    if (runtime.implementation_ == nullptr) {
        return false;
    }

    auto& implementation = *runtime.implementation_;
    std::scoped_lock lock{implementation.table_gate};
    auto* owner = implementation.find_live_owner(target);
    if (owner == nullptr) {
        return false;
    }
    owner->facts_requested = seed.requested;
    owner->facts_installed = seed.installed;
    owner->metadata_fault = false;
    return true;
}
} // namespace inputnext
