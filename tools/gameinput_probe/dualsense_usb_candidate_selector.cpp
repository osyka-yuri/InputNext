#include "dualsense_usb_candidate_selector.hpp"

#include "rumble_session.hpp"
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] bool is_dualsense_model(const metadata_device_model model) noexcept {
    return model == metadata_device_model::dualsense ||
           model == metadata_device_model::dualsense_edge;
}

} // namespace

void dualsense_usb_candidate_selector::retain_current(const connection_inventory& inventory) {
    std::vector<qualification_entry> retained;
    retained.reserve(inventory.size);
    for (std::size_t index = 0U; index < inventory.size; ++index) {
        const auto target = inventory.entries[index].target;
        const auto found = std::ranges::find_if(entries_,
            [target](const auto& entry) { return entry.target == target; });
        if (found != entries_.end()) {
            retained.push_back(*found);
        } else {
            retained.push_back(qualification_entry{.target = target, .observation = {}});
        }
    }
    entries_ = std::move(retained);
}

dualsense_usb_candidate_selector::qualification_entry& dualsense_usb_candidate_selector::for_target(
    const connection_target target,
    const std::uint64_t catalogue_generation) {
    const auto found = std::ranges::find_if(entries_,
        [target](const auto& entry) { return entry.target == target; });
    if (found == entries_.end()) {
        entries_.push_back({.target = target, .observation = {}});
        auto& entry = entries_.back();
        entry.observation.select(target, catalogue_generation);
        return entry;
    }
    found->observation.select(target, catalogue_generation);
    return *found;
}

std::optional<dualsense_usb_candidate>
dualsense_usb_candidate_selector::inspect(rumble_session& session, const maintenance_time now) {
    const auto inventory = session.inventory();
    retain_current(inventory);
    for (std::size_t index = 0U; index < inventory.size; ++index) {
        const auto target = inventory.entries[index].target;
        const auto description = session.describe(target);
        if (!description.has_value() || description->target != target ||
            !description->model.has_value() || !is_dualsense_model(*description->model)) {
            continue;
        }

        if (qualify_cached(session, target, description->catalogue_generation, now)) {
            return dualsense_usb_candidate{.target = target,
                .catalogue_generation = description->catalogue_generation,
                .model = *description->model};
        }
    }
    return std::nullopt;
}

bool dualsense_usb_candidate_selector::is_eligible(rumble_session& session,
    const connection_target target,
    const std::uint64_t catalogue_generation,
    const maintenance_time now) {
    retain_current(session.inventory());
    return qualify_cached(session, target, catalogue_generation, now);
}

bool dualsense_usb_candidate_selector::qualify_cached(rumble_session& session,
    const connection_target target,
    const std::uint64_t catalogue_generation,
    const maintenance_time now) {
    auto& cached = for_target(target, catalogue_generation);
    if (cached.observation.due(now)) {
        const auto query_started = std::max(now, std::chrono::steady_clock::now());
        metadata_result<bool> qualified = std::unexpected{metadata_error::resource_exhausted};
        try {
            qualified = session.qualify_dualsense_usb(target, catalogue_generation);
        } catch (...) {
            qualified = std::unexpected{metadata_error::resource_exhausted};
        }
        const auto completed = std::max(query_started, std::chrono::steady_clock::now());
        cached.observation.observe(qualified, {.started = query_started, .completed = completed});
    }
    return cached.observation.eligible(std::max(now, std::chrono::steady_clock::now()));
}

} // namespace inputnext::gameinput_detail
