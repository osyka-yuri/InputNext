#include <inputnext/gameinput_xinput.hpp>

#include <inputnext/device_metadata.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <span>
#include <system_error>

namespace inputnext {
namespace {

[[nodiscard]] std::uint64_t system_qpc_frequency() {
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(),
            "QueryPerformanceFrequency");
    }
    if (frequency.QuadPart <= 0) {
        throw std::system_error(static_cast<int>(ERROR_INVALID_DATA),
            std::system_category(),
            "QueryPerformanceFrequency returned a non-positive frequency");
    }
    return static_cast<std::uint64_t>(frequency.QuadPart);
}

[[nodiscard]] history_failure history_failure_for_exception(history_error code) noexcept {
    return history_failure{.code = code};
}

[[nodiscard]] bool eligible(const gameinput_connection& connection) noexcept {
    return connection.target.valid() && connection.pad_known &&
           (connection.health == connection_health::callback_driven ||
               connection.health == connection_health::polling_fallback);
}

} // namespace

gameinput_xinput::gameinput_xinput(gameinput_runtime& runtime, focus::observer& focus)
    : gameinput_xinput(runtime, focus, system_qpc_frequency()) {
}

gameinput_xinput::gameinput_xinput(gameinput_runtime& runtime,
    focus::observer& focus,
    std::uint64_t qpc_frequency)
    : runtime_(runtime),
      presentation_(focus, *this, *this, qpc_frequency) {
}

gameinput_xinput::~gameinput_xinput() {
    presentation_.close();
}

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

void gameinput_xinput::maintain_events(history_time now) {
    for (DWORD user_index = 0U; user_index < 4U; ++user_index) {
        presentation_.pump_events(user_index, now);
    }
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

history_result<history_reader_handle> gameinput_xinput::open_history(std::uint32_t position,
    connection_target target,
    history_time now) noexcept {
    try {
        return runtime_.open_xinput_history(position, target, now);
    } catch (const std::bad_alloc&) {
        return std::unexpected(history_failure_for_exception(history_error::capacity_exceeded));
    } catch (...) {
        return std::unexpected(history_failure_for_exception(history_error::provider_failure));
    }
}

history_batch gameinput_xinput::read_history(history_reader_handle reader,
    history_time now) noexcept {
    try {
        return runtime_.read_history(reader, now);
    } catch (const std::bad_alloc&) {
        history_batch batch{};
        batch.domain.target = reader.target;
        batch.failure = history_failure_for_exception(history_error::capacity_exceeded);
        return batch;
    } catch (...) {
        history_batch batch{};
        batch.domain.target = reader.target;
        batch.failure = history_failure_for_exception(history_error::provider_failure);
        return batch;
    }
}

history_result<void> gameinput_xinput::close_history(history_reader_handle reader) noexcept {
    try {
        return runtime_.close_history(reader);
    } catch (const std::bad_alloc&) {
        return std::unexpected(history_failure_for_exception(history_error::capacity_exceeded));
    } catch (...) {
        return std::unexpected(history_failure_for_exception(history_error::provider_failure));
    }
}

} // namespace inputnext
