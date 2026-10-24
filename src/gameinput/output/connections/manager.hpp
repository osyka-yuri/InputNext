#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::gameinput_detail {

class runtime_output_access;

enum class connection_control_phase : std::uint8_t {
    accepting,
    admission_closed,
    stopping,
    needs_executor_shutdown,
    stopped,
};

enum class connection_control_operation : std::uint8_t {
    describe,
    qualify_backend,
    reserve,
    create_sink,
    register_backend,
    progress_backend,
    retire_backend,
    acknowledge_retirement,
    worker_exception,
};

struct connection_control_failure final {
    connection_control_operation operation{connection_control_operation::worker_exception};
    connection_target target{};
    std::optional<output::backend_handle> backend{std::nullopt};
    std::optional<gameinput_failure> runtime_failure{std::nullopt};
    std::optional<native_failure> native_failure{std::nullopt};
    std::optional<output::execution_error> executor_failure{std::nullopt};
    std::optional<metadata_error> metadata_failure{std::nullopt};
};

struct connection_control_report final {
    connection_control_phase phase{connection_control_phase::accepting};
    bool admission_quiescent{};
    bool control_finished{};
    bool control_joined{};
    bool all_retirements_acknowledged{};
    bool executor_shutdown_returned{};
    std::size_t candidate_obligations{};
    std::size_t backend_obligations{};
    std::size_t pending_retirements{};
    std::optional<connection_control_failure> failure{std::nullopt};
};

// Sole owner of managed backend registration and retirement for one borrowed executor. The
// runtime output access and executor must outlive this object; their owners must quiesce every
// other executor caller before shutdown().
class gameinput_output_connections final {
public:
    [[nodiscard]] static gameinput_result<std::unique_ptr<gameinput_output_connections>>
    create(runtime_output_access& runtime, output::executor& executor);
    ~gameinput_output_connections() noexcept;

    gameinput_output_connections(const gameinput_output_connections&) = delete;
    gameinput_output_connections& operator=(const gameinput_output_connections&) = delete;
    gameinput_output_connections(gameinput_output_connections&&) = delete;
    gameinput_output_connections& operator=(gameinput_output_connections&&) = delete;

    [[nodiscard]] gameinput_result<void> synchronize(const connection_inventory& inventory);
    void close_admission() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] connection_control_report progress() const noexcept;
    // Control-worker snapshot; pending native work may be newer.
    [[nodiscard]] std::optional<output::backend_progress> backend_progress(
        connection_target target) const noexcept;
    // Queries the exact managed backend outside the control gate.
    [[nodiscard]] std::optional<output::backend_progress> query_backend_progress(
        connection_target target) const noexcept;
    void shutdown() noexcept;

private:
    struct implementation;

    explicit gameinput_output_connections(std::unique_ptr<implementation> state) noexcept;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::gameinput_detail
