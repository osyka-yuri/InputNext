#include "worker.hpp"

#include "history_observer.hpp"
#include "report.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <ostream>
#include <system_error>
#include <thread>


#include <filesystem>

#include <stdexcept>

#include <string>

#include <vector>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(256U);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(),
                "GetModuleFileNameW");
        }
        if (length < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), length}};
        }
        if (buffer.size() >= 32'768U) {
            throw std::length_error("The executable path exceeds the Windows path limit.");
        }
        buffer.resize(buffer.size() * 2U);
    }
}

[[nodiscard]] runtime_id make_runtime_id() {
    GUID guid{};
    const HRESULT result = CoCreateGuid(&guid);
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), "CoCreateGuid");
    }

    runtime_id identity{};
    static_assert(sizeof(guid) == identity.bytes.size());
    std::memcpy(identity.bytes.data(), &guid, sizeof(guid));
    return identity;
}

struct metadata_query_entry final {
    connection_target target{};
    maintenance_time next_attempt{maintenance_time::min()};
};

struct metadata_query_schedule final {
    std::array<metadata_query_entry, gameinput_live_connection_capacity> entries{};
    std::size_t size{};

    void retain_current_targets(const connection_inventory& devices) noexcept {
        std::array<metadata_query_entry, gameinput_live_connection_capacity> current{};
        std::size_t current_size{};
        for (std::size_t index = 0U; index < devices.size; ++index) {
            const auto target = devices.entries[index].target;
            const auto end = entries.begin() + static_cast<std::ptrdiff_t>(size);
            const auto found = std::ranges::find_if(entries.begin(),
                end,
                [target](const metadata_query_entry& entry) { return entry.target == target; });
            if (found != end) {
                current[current_size++] = *found;
            } else {
                current[current_size++] = {.target = target};
            }
        }
        entries = current;
        size = current_size;
    }

    [[nodiscard]] bool is_due(connection_target target, maintenance_time now) const noexcept {
        const auto end = entries.begin() + static_cast<std::ptrdiff_t>(size);
        const auto found = std::ranges::find_if(entries.begin(),
            end,
            [target](const metadata_query_entry& entry) { return entry.target == target; });
        return found != end && now >= found->next_attempt;
    }

    void defer(connection_target target, maintenance_time now) noexcept {
        const auto end = entries.begin() + static_cast<std::ptrdiff_t>(size);
        const auto found = std::ranges::find_if(entries.begin(),
            end,
            [target](const metadata_query_entry& entry) { return entry.target == target; });
        if (found != end) {
            found->next_attempt = now + std::chrono::seconds{1};
        }
    }
};

struct probe_observation final {
    bool saw_device{};
    bool saw_reading{};
    bool saw_history{};
    bool history_closed{true};
    metadata_query_schedule metadata_queries{};
};

void observe_connection(gameinput_runtime& runtime,
    const gameinput_connection& connection,
    maintenance_time now,
    bool report,
    bool& queried_this_pass,
    probe_observation& observation,
    std::ostream& output) {
    const auto sample = connection.reader.read();
    observation.saw_reading = observation.saw_reading || sample.has_value();
    if (report) {
        output << format_device(connection, sample) << '\n';
    }

    if (!queried_this_pass && observation.metadata_queries.is_due(connection.target, now)) {
        queried_this_pass = true;
        const auto metadata = runtime.query_metadata(connection.target);
        // OS metadata queries are synchronous; limit work to one coordinator pass and defer one second after completion.
        observation.metadata_queries.defer(connection.target, maintenance_time::clock::now());
        output << format_metadata_result(connection.target, metadata) << '\n';
    }
}

void observe_connections(gameinput_runtime& runtime,
    const connection_inventory& devices,
    maintenance_time now,
    bool report,
    probe_observation& observation,
    std::ostream& output) {
    observation.saw_device = observation.saw_device || devices.size != 0U;
    observation.metadata_queries.retain_current_targets(devices);
    bool queried_this_pass = false;
    for (std::size_t index = 0U; index < devices.size; ++index) {
        observe_connection(runtime,
            devices.entries[index],
            now,
            report,
            queried_this_pass,
            observation,
            output);
    }
}

[[nodiscard]] int report_completion(const probe_options& options,
    const probe_observation& observation,
    const gameinput_stop_report& report,
    std::ostream& output) {
    output << format_stop_report(report) << '\n';
    output.flush();
    if (!report.drained || !observation.history_closed) {
        return 3;
    }

    const auto requirements = format_requirements({.device_required = options.require_device,
        .reading_required = options.require_reading,
        .device_observed = observation.saw_device,
        .reading_observed = observation.saw_reading,
        .history_required = options.require_history,
        .history_observed = observation.saw_history});
    output << requirements.line << '\n';
    return requirements.met ? 0 : 4;
}

} // namespace

int run_probe_worker(const probe_options& options, std::ostream& output) {
    try {
        const auto library = executable_path().parent_path() / L"GameInputRedist.dll";
        device_registry registry{make_runtime_id()};
        gameinput_runtime runtime{registry, gameinput_options{.runtime_library = library}};

        auto started = runtime.start(maintenance_time::clock::now());
        if (!started.has_value()) {
            output << format_start_failure(started.error()) << '\n'
                   << format_stop_report(runtime.stop(maintenance_time::clock::now())) << '\n';
            output.flush();
            return 3;
        }
        output << "start=ok runtime_library=app-local\n";
        if (options.history || options.require_history) {
            output << "history_enabled=true\n";
        }

        probe_observation observation{};
        history_observer history{runtime};
        auto next_report = maintenance_time::clock::now();
        const auto deadline = next_report + std::chrono::milliseconds{options.duration_ms};

        while (maintenance_time::clock::now() < deadline) {
            const auto now = maintenance_time::clock::now();
            runtime.maintain(now);
            const auto devices = runtime.connections();
            observe_connections(runtime, devices, now, now >= next_report, observation, output);
            if (options.history || options.require_history) {
                history.observe(devices,
                    maintenance_time::clock::now(),
                    now >= next_report,
                    output);
            }
            if (now >= next_report) {
                output.flush();
                next_report = now + std::chrono::milliseconds{250};
            }

            const auto opportunity = maintenance_time::clock::now() + std::chrono::milliseconds{4};
            std::this_thread::sleep_until(std::min(deadline, opportunity));
        }

        observation.saw_history = history.native_history_observed();
        observation.history_closed = history.close_all(output);
        const auto report = runtime.stop(maintenance_time::clock::now());
        return report_completion(options, observation, report, output);
    } catch (const std::exception& error) {
        output << "worker=failed message=" << error.what() << '\n';
        return 3;
    }
}

} // namespace inputnext::gameinput_detail
