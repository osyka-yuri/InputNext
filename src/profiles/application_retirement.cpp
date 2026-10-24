#include "application.hpp"

#include <inputnext/output.hpp>
#include <inputnext/xinput.hpp>

#include <chrono>
#include <optional>
#include <thread>

namespace inputnext::profiles {
namespace {

void wait_to_retry() noexcept {
    try {
        std::this_thread::sleep_for(std::chrono::milliseconds{4});
    } catch (...) {
        std::this_thread::yield();
    }
}

} // namespace

application::~application() noexcept {
    for (;;) {
        try {
            if (close().pending_contexts == 0U) {
                return;
            }
        } catch (...) {
            // Keep exact ownership after an interrupted cleanup operation.
            wait_to_retry();
            continue;
        }
        wait_to_retry();
    }
}

bool application::retire(std::optional<obligation>& owned,
    const xinput::assignment& assignment,
    application_report& report) {
    if (!owned.has_value()) {
        return true;
    }
    const auto context = owned->context;
    if (!owned->receipt.has_value()) {
        const auto closed = producer_.close(context, context.identity.id.value);
        if (!closed.has_value()) {
            if (closed.error() == output::error::stale_context) {
                owned.reset();
                return true;
            }
            record_failure(report,
                application_operation::close,
                closed.error(),
                assignment,
                context);
            return false;
        }
        owned->receipt = *closed;
    }

    const auto acknowledged = producer_.acknowledge(*owned->receipt);
    if (acknowledged.has_value()) {
        owned.reset();
        return true;
    }
    if (acknowledged.error() == output::error::stale_context) {
        const auto retained = producer_.close(context, context.identity.id.value);
        if (!retained.has_value() && retained.error() == output::error::stale_context) {
            owned.reset();
            return true;
        }
        if (!retained.has_value()) {
            record_failure(report,
                application_operation::close,
                retained.error(),
                assignment,
                context);
            return false;
        }
        owned->receipt = *retained;
    }
    record_failure(report,
        application_operation::acknowledge,
        acknowledged.error(),
        assignment,
        context);
    return false;
}

bool application::drain(position& entry, application_report& report) {
    if (!entry.assignment.has_value()) {
        return true;
    }
    const bool retired = retire(entry.retiring, *entry.assignment, report);
    const bool current = retire(entry.current, *entry.assignment, report);
    if (retired && current) {
        entry.assignment.reset();
        return true;
    }
    return false;
}

void application::drain_all(application_report& report) {
    for (auto& entry : positions_) {
        static_cast<void>(drain(entry, report));
    }
}

application_report application::close() {
    closed_ = true;
    application_report report;
    drain_all(report);
    return finish(report);
}

} // namespace inputnext::profiles
