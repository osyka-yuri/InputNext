#include "gameinput_probe/input/worker.hpp"
#include "gameinput_probe/options.hpp"

#include "gameinput_probe/input/history/observer.hpp"
#include "gameinput_probe/input/xinput/event_observer.hpp"
#include "gameinput_probe/input/xinput/observer.hpp"
#include "gameinput_probe/process/executable_path.hpp"
#include "gameinput_probe/report.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <ostream>
#include <system_error>
#include <thread>

namespace inputnext::gameinput_detail {
namespace {

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

class runtime_cleanup final {
public:
    runtime_cleanup(gameinput_runtime& runtime,
        std::unique_ptr<focus::observer>& focus,
        std::unique_ptr<gameinput_xinput>& presentation,
        std::unique_ptr<xinput_event_observer>& events) noexcept
        : runtime_(runtime),
          focus_(focus),
          presentation_(presentation),
          events_(events) {}

    ~runtime_cleanup() noexcept {
        if (!stopped_) {
            static_cast<void>(stop());
        }
    }

    runtime_cleanup(const runtime_cleanup&) = delete;
    runtime_cleanup& operator=(const runtime_cleanup&) = delete;
    runtime_cleanup(runtime_cleanup&&) = delete;
    runtime_cleanup& operator=(runtime_cleanup&&) = delete;

    void stop_presenters() {
        if (presenters_stopped_) {
            return;
        }
        if (events_) {
            events_->stop();
        }
        if (presentation_) {
            presentation_->close();
        }
        presenters_stopped_ = true;
    }

    [[nodiscard]] gameinput_stop_report stop() {
        if (stopped_) {
            return report_;
        }
        stop_presenters();
        report_ = runtime_.stop(maintenance_time::clock::now());
        if (report_.drained && focus_) {
            focus_->stop();
        }
        stopped_ = true;
        return report_;
    }

private:
    gameinput_runtime& runtime_;
    std::unique_ptr<focus::observer>& focus_;
    std::unique_ptr<gameinput_xinput>& presentation_;
    std::unique_ptr<xinput_event_observer>& events_;
    gameinput_stop_report report_{};
    bool presenters_stopped_{};
    bool stopped_{};
};

class history_cleanup final {
public:
    history_cleanup(history_observer& history,
        runtime_cleanup& cleanup,
        std::ostream& output) noexcept
        : history_(history),
          cleanup_(cleanup),
          output_(output) {}

    ~history_cleanup() noexcept {
        if (!closed_) {
            try {
                static_cast<void>(close());
            } catch (...) {
                clean_ = false;
                static_cast<void>(std::fputs("history_cleanup=failed\n", stderr));
            }
        }
    }

    history_cleanup(const history_cleanup&) = delete;
    history_cleanup& operator=(const history_cleanup&) = delete;
    history_cleanup(history_cleanup&&) = delete;
    history_cleanup& operator=(history_cleanup&&) = delete;

    [[nodiscard]] bool close() {
        if (!closed_) {
            closed_ = true;
            cleanup_.stop_presenters();
            clean_ = history_.close_all(output_);
        }
        return clean_;
    }

private:
    history_observer& history_;
    runtime_cleanup& cleanup_;
    std::ostream& output_;
    bool clean_{true};
    bool closed_{};
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

[[nodiscard]] bool observe_connections(gameinput_runtime& runtime,
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
    return queried_this_pass;
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

void run_maintenance_loop(const probe_options& options,
    gameinput_runtime& runtime,
    gameinput_xinput* presentation,
    xinput_observer* xinput,
    xinput_event_observer* xinput_events,
    history_observer& history,
    probe_observation& observation,
    std::ostream& output) {
    auto next_report = maintenance_time::clock::now();
    const auto deadline =
        next_report + std::chrono::milliseconds{options.duration_ms.value_or(10'000U)};

    while (maintenance_time::clock::now() < deadline) {
        const auto now = maintenance_time::clock::now();
        const bool report = now >= next_report;
        runtime.maintain(now);
        const auto devices = runtime.connections();
        if (presentation != nullptr) {
            const auto synchronized = presentation->synchronize(devices);
            if (!synchronized.has_value() && report) {
                output << "xinput_sync=failed error_code="
                       << static_cast<unsigned int>(synchronized.error()) << '\n';
            }
            if (xinput_events != nullptr) {
                presentation->maintain_events(now);
            }
        }
        const bool metadata_queried =
            observe_connections(runtime, devices, now, report, observation, output);
        if (presentation != nullptr) {
            xinput->observe(devices, now, report, !metadata_queried, output);
            if (xinput_events != nullptr) {
                xinput_events->observe(now, report, output);
            }
        }
        if (options.history || options.require_history) {
            history.observe(devices, maintenance_time::clock::now(), report, output);
        }
        if (report) {
            output.flush();
            next_report = now + std::chrono::milliseconds{250};
        }

        const auto opportunity = maintenance_time::clock::now() + std::chrono::milliseconds{4};
        std::this_thread::sleep_until(std::min(deadline, opportunity));
    }
}

} // namespace

int run_probe_worker(const probe_options& options, std::ostream& output) {
    try {
        const auto library = current_executable_path().parent_path() / L"GameInputRedist.dll";
        device_registry registry{make_runtime_id()};
        std::unique_ptr<focus::observer> focus;
        gameinput_runtime runtime{registry, gameinput_options{.runtime_library = library}};
        std::unique_ptr<gameinput_xinput> presentation;
        std::unique_ptr<xinput_observer> xinput;
        std::unique_ptr<xinput_event_observer> xinput_events;
        runtime_cleanup cleanup{runtime, focus, presentation, xinput_events};

        auto started = runtime.start(maintenance_time::clock::now());
        if (!started.has_value()) {
            output << format_start_failure(started.error()) << '\n'
                   << format_stop_report(cleanup.stop()) << '\n';
            output.flush();
            return 3;
        }
        output << "start=ok runtime_library=app-local\n";
        if (options.history || options.require_history) {
            output << "history_enabled=true\n";
        }
        if (options.xinput || options.xinput_events) {
            focus = std::make_unique<focus::observer>(make_runtime_id(),
                focus::make_current_process_source());
            presentation = std::make_unique<gameinput_xinput>(runtime, *focus);
            xinput = std::make_unique<xinput_observer>(*presentation, *focus);
            output << "xinput_enabled=true focus_source=current_process\n";
        }
        if (options.xinput_events) {
            xinput_events = std::make_unique<xinput_event_observer>(*presentation);
            output << "xinput_events_enabled=true\n";
        }

        probe_observation observation{};
        history_observer history{runtime};
        history_cleanup close_history{history, cleanup, output};
        run_maintenance_loop(options,
            runtime,
            presentation.get(),
            xinput.get(),
            xinput_events.get(),
            history,
            observation,
            output);

        observation.saw_history = history.native_history_observed();
        observation.history_closed = close_history.close();
        if (xinput_events) {
            xinput_events->report_shutdown(output);
        }
        const auto report = cleanup.stop();
        return report_completion(options, observation, report, output);
    } catch (const std::exception& error) {
        output << "worker=failed message=" << error.what() << '\n';
        return 3;
    }
}

} // namespace inputnext::gameinput_detail
