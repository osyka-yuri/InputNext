#include "history_observer.hpp"

#include "history_report.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <ostream>
#include <span>

namespace inputnext::gameinput_detail {

history_observer::history_observer(gameinput_runtime& runtime) noexcept : runtime_(runtime) {
}

void history_observer::close(entry& observed, std::ostream& output) {
    const auto reader = observed.reader.value_or(history_reader_handle{});
    const auto target = reader.target;
    const auto closed = runtime_.close_history(reader);
    output << format_history_operation("close", target, closed) << '\n';
    closed_cleanly_ = closed_cleanly_ && closed.has_value();
    observed = {};
}

void history_observer::read(entry& observed,
    maintenance_time now,
    bool report,
    std::ostream& output) {
    const auto reader = observed.reader.value_or(history_reader_handle{});
    const auto batch = runtime_.read_history(reader, now);
    observed.evidence.observe(batch);
    observed_native_history_ =
        observed_native_history_ || observed.evidence.native_history_observed();
    for (std::size_t index = 0U; index < batch.count; ++index) {
        const auto& record = batch.records[index];
        if (record.baseline || observed.buttons != record.sample.state.buttons ||
            observed.guide_known != record.sample.guide_known) {
            output << format_history_record(batch.domain, record) << '\n';
        }
        observed.buttons = record.sample.state.buttons;
        observed.guide_known = record.sample.guide_known;
    }
    const auto failure = batch.failure.value_or(history_failure{});
    if (report || batch.reset != history_reset::none ||
        (batch.failure.has_value() && failure.code != history_error::busy &&
            failure.code != history_error::no_data)) {
        output << format_history_batch(reader.target, batch, observed.evidence) << '\n';
    }
}

void history_observer::observe(const connection_inventory& connections,
    maintenance_time now,
    bool report,
    std::ostream& output) {
    const auto live = std::span{connections.entries}.first(connections.size);
    for (auto& observed : entries_) {
        if (observed.reader.has_value() &&
            std::ranges::none_of(live, [&](const gameinput_connection& connection) {
                return connection.target ==
                       observed.reader.value_or(history_reader_handle{}).target;
            })) {
            close(observed, output);
        }
    }

    for (const auto& connection : live) {
        auto found = std::ranges::find_if(entries_, [&](const entry& observed) {
            return observed.reader.has_value() &&
                   observed.reader.value_or(history_reader_handle{}).target == connection.target;
        });
        if (found == entries_.end()) {
            found = std::ranges::find_if(entries_,
                [](const entry& observed) { return !observed.reader.has_value(); });
            if (found == entries_.end()) {
                continue;
            }
            const auto opened = runtime_.open_history(connection.target, now);
            if (!opened.has_value()) {
                if (report) {
                    output << format_history_operation("open",
                                  connection.target,
                                  std::unexpected(opened.error()))
                           << '\n';
                }
                continue;
            }
            found->reader = *opened;
            output << format_history_operation("open", connection.target, {}) << '\n';
        }
        read(*found, now, report, output);
    }
}

bool history_observer::close_all(std::ostream& output) {
    for (auto& observed : entries_) {
        if (observed.reader.has_value()) {
            close(observed, output);
        }
    }
    return closed_cleanly_;
}

bool history_observer::native_history_observed() const noexcept {
    return observed_native_history_;
}

} // namespace inputnext::gameinput_detail
