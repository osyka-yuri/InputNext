#include "xinput_observer.hpp"

#include "xinput_report.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] std::optional<std::uint64_t> latest_revision(const connection_inventory& connections,
    connection_target target) {
    const auto size = std::min(connections.size, connections.entries.size());
    const auto end = connections.entries.begin() + static_cast<std::ptrdiff_t>(size);
    const auto found = std::ranges::find_if(connections.entries.begin(),
        end,
        [target](const auto& item) { return item.target == target; });
    if (found == end) {
        return std::nullopt;
    }
    const auto sample = found->reader.read();
    return sample.has_value() ? std::optional{sample->sample.revision} : std::nullopt;
}

} // namespace

xinput_observer::xinput_observer(gameinput_xinput& presentation, focus::observer& focus) noexcept
    : presentation_(presentation),
      focus_(focus) {
}

void xinput_observer::observe(const connection_inventory& connections,
    maintenance_time now,
    bool report,
    bool allow_metadata_query,
    std::ostream& output) {
    const auto focus_snapshot = focus_.published();
    if (report) {
        output << format_xinput_focus(focus_snapshot) << '\n';
    }

    const auto positions = presentation_.presentation().positions();
    std::optional<std::size_t> due_capabilities;
    for (std::size_t index = 0U; index < positions.size(); ++index) {
        const auto& assignment = positions[index];
        auto& schedule = capabilities_[index];
        if (!assignment.has_value()) {
            schedule = {};
            continue;
        }

        if (schedule.assignment != assignment) {
            schedule.assignment = assignment;
            schedule.next_attempt = maintenance_time::min();
        }
        if (allow_metadata_query && !due_capabilities.has_value() && now >= schedule.next_attempt) {
            due_capabilities = index;
        }

        XINPUT_STATE standard{};
        XINPUT_STATE extended{};
        const DWORD standard_result = presentation_.presentation().get_state(xinput::flavor::v1_4,
            assignment->user_index,
            &standard,
            false);
        const DWORD extended_result = presentation_.presentation().get_state(xinput::flavor::v1_4,
            assignment->user_index,
            &extended,
            true);
        if (report) {
            output << format_xinput_state(*assignment,
                          latest_revision(connections, assignment->target),
                          focus_snapshot,
                          standard_result,
                          standard,
                          extended_result,
                          extended)
                   << '\n';
        }
    }

    if (due_capabilities.has_value()) {
        const auto index = due_capabilities.value_or(positions.size());
        if (index < positions.size()) {
            const auto& assignment = positions[index];
            if (assignment.has_value()) {
                xinput::capabilities_ex capabilities{};
                const DWORD result =
                    presentation_.presentation().get_capabilities_ex(xinput::flavor::v1_4,
                        assignment->user_index,
                        0U,
                        &capabilities);
                capabilities_[index].next_attempt =
                    maintenance_time::clock::now() + std::chrono::seconds{1};
                output << format_xinput_capabilities(*assignment, result, capabilities) << '\n';
            }
        }
    }
}

} // namespace inputnext::gameinput_detail
