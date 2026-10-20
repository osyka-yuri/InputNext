#include <inputnext/gameinput_xinput.hpp>

#include <inputnext/device_metadata.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>

namespace inputnext {
namespace {

[[nodiscard]] bool eligible(const gameinput_connection& connection) noexcept {
    return connection.target.valid() && connection.pad_known &&
           (connection.health == connection_health::callback_driven ||
               connection.health == connection_health::polling_fallback);
}

} // namespace

gameinput_xinput::gameinput_xinput(gameinput_runtime& runtime, focus::observer& focus)
    : runtime_(runtime),
      presentation_(focus, *this) {
}

gameinput_xinput::~gameinput_xinput() = default;

result<void> gameinput_xinput::synchronize(const connection_inventory& inventory) {
    const auto size = std::min(inventory.size, inventory.entries.size());
    const auto connections = std::span{inventory.entries}.first(size);
    auto current = presentation_.positions();
    std::array<bool, gameinput_live_connection_capacity> retained{};
    std::optional<error> first_error;

    for (auto& occupant : current) {
        if (!occupant.has_value()) {
            continue;
        }
        const auto found = std::ranges::find_if(connections, [&](const auto& connection) {
            return connection.target == occupant->target && eligible(connection);
        });
        if (found != connections.end()) {
            retained[static_cast<std::size_t>(found - connections.begin())] = true;
            continue;
        }

        const auto released = presentation_.unassign(*occupant);
        if (released.has_value()) {
            occupant.reset();
        } else if (!first_error.has_value()) {
            first_error = released.error();
        }
    }

    for (std::size_t index = 0U; index < connections.size(); ++index) {
        const auto& connection = connections[index];
        if (!eligible(connection) || retained[index]) {
            continue;
        }

        const auto free = std::ranges::find_if(current,
            [](const auto& occupant) { return !occupant.has_value(); });
        if (free == current.end()) {
            break;
        }

        auto assigned = presentation_.assign(connection.target, connection.reader);
        if (!assigned.has_value()) {
            if (!first_error.has_value()) {
                first_error = assigned.error();
            }
            continue;
        }
        *free = *assigned;
    }

    if (first_error.has_value()) {
        return std::unexpected(*first_error);
    }
    return {};
}

xinput::service& gameinput_xinput::presentation() noexcept {
    return presentation_;
}

void gameinput_xinput::close() {
    presentation_.close();
}

metadata_result<metadata_snapshot> gameinput_xinput::query(connection_target target) {
    return runtime_.query_metadata(target);
}

} // namespace inputnext
