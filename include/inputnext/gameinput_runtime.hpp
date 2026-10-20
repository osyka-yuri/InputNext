#pragma once

#include "device_metadata.hpp"
#include "device_registry.hpp"
#include "history.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>

#include <inputnext/device_identity.hpp>
#include <inputnext/input_binding.hpp>

namespace inputnext {

using maintenance_time = std::chrono::steady_clock::time_point;

enum class gameinput_error : std::uint8_t {
    invalid_argument,
    already_started,
    closed,
    load_failed,
    api_unavailable,
    registration_failed,
    resource_exhausted,
    native_failure,
};

struct gameinput_failure final {
    gameinput_error code{gameinput_error::native_failure};
    std::uint32_t native_code{};
};

template <typename T>
using gameinput_result = std::expected<T, gameinput_failure>;

struct gameinput_options final {
    std::filesystem::path runtime_library;
};

enum class connection_health : std::uint8_t {
    discovering,
    callback_driven,
    polling_fallback,
    retirement_pending,
    failed,
};

struct gameinput_connection final {
    connection_target target{};
    read_binding reader{};
    connection_health health{connection_health::discovering};
    bool pad_known{};
    bool guide_known{};
};

inline constexpr std::size_t gameinput_live_connection_capacity = 16U;

struct connection_inventory final {
    std::array<gameinput_connection, gameinput_live_connection_capacity> entries{};
    std::size_t size{};
};

struct gameinput_stop_report final {
    bool drained{};
    std::size_t pending_callback_owners{};
    std::size_t active_metadata_queries{};
    std::size_t active_history_calls{};
    std::size_t pending_history_owners{};
};

namespace gameinput_detail {
class runtime_factory;
}

class gameinput_xinput;

// Caller-driven GameInput lifecycle with latest-state and bounded history consumers. The registry
// must outlive this runtime, all returned read bindings, and every admitted query. No worker is
// started internally.
class gameinput_runtime final {
public:
    gameinput_runtime(device_registry& registry, gameinput_options options);
    ~gameinput_runtime() noexcept;

    gameinput_runtime(const gameinput_runtime&) = delete;
    gameinput_runtime& operator=(const gameinput_runtime&) = delete;
    gameinput_runtime(gameinput_runtime&&) = delete;
    gameinput_runtime& operator=(gameinput_runtime&&) = delete;

    [[nodiscard]] gameinput_result<void> start(maintenance_time now);
    void maintain(maintenance_time now);
    [[nodiscard]] std::optional<maintenance_time> next_deadline() const;
    [[nodiscard]] connection_inventory connections() const;
    [[nodiscard]] metadata_result<metadata_snapshot> query_metadata(connection_target target);
    [[nodiscard]] history_result<history_reader_handle> open_history(connection_target target,
        history_time now);
    [[nodiscard]] history_batch read_history(history_reader_handle reader, history_time now);
    [[nodiscard]] history_result<void> close_history(history_reader_handle reader);
    [[nodiscard]] gameinput_stop_report stop(maintenance_time now);

private:
    struct implementation;
    explicit gameinput_runtime(std::unique_ptr<implementation> implementation) noexcept;

    friend class gameinput_detail::runtime_factory;
    friend class gameinput_xinput;

    std::unique_ptr<implementation> implementation_;

    [[nodiscard]] history_result<history_reader_handle>
    open_xinput_history(std::uint32_t position, connection_target target, history_time now);
};

} // namespace inputnext
