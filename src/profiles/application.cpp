#include "application.hpp"

#include "model.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/output.hpp>
#include <inputnext/xinput.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace inputnext::profiles {
namespace {

[[nodiscard]] bool empty(const profile& value) noexcept {
    return !value.left_trigger.has_value() && !value.right_trigger.has_value();
}

[[nodiscard]] output::submit_request request_for(const profile& value) noexcept {
    output::submit_request request{.sequence = 1U};
    if (value.left_trigger.has_value()) {
        request.selected_channels |= output::left_trigger_mask;
        request.values.left_trigger = *value.left_trigger;
    }
    if (value.right_trigger.has_value()) {
        request.selected_channels |= output::right_trigger_mask;
        request.values.right_trigger = *value.right_trigger;
    }
    return request;
}

[[nodiscard]] bool terminal_error(const output::error error) noexcept {
    switch (error) {
    case output::error::not_connected:
    case output::error::resources:
    case output::error::capacity_exceeded:
    case output::error::stale_context:
    case output::error::revoked:
        return false;
    case output::error::invalid_argument:
    case output::error::identity_exhausted:
    case output::error::sequence_rejected:
    case output::error::revision_exhausted:
    case output::error::logical_version_exhausted:
    case output::error::stale_revision:
    case output::error::expired:
    case output::error::deadline_overflow:
    case output::error::busy:
    case output::error::stale_lease:
    case output::error::stale_audio:
        return true;
    }
    return true;
}

} // namespace

application::application(output::detail::internal_producer producer, xinput::service& presentation)
    : producer_(std::move(producer)),
      presentation_(presentation) {
}

application_report application::finish(application_report report) const noexcept {
    for (const auto& entry : positions_) {
        report.pending_contexts += entry.current.has_value() ? 1U : 0U;
        report.pending_contexts += entry.retiring.has_value() ? 1U : 0U;
    }
    report.terminal = closed_;
    if (terminal_failure_.has_value()) {
        report.failure = terminal_failure_;
    }
    return report;
}

void application::record_failure(application_report& report,
    const application_operation operation,
    const output::error error,
    const xinput::assignment& assignment,
    const output::internal_context context) noexcept {
    const application_failure failure{.operation = operation,
        .code = error,
        .assignment = assignment,
        .context = context};
    if (!report.failure.has_value()) {
        report.failure = failure;
    }
    if (terminal_error(error)) {
        closed_ = true;
        if (!terminal_failure_.has_value()) {
            terminal_failure_ = failure;
        }
    }
}

application_report application::replace_profile(const profile& desired) {
    if (closed_) {
        return finish({.failure = application_failure{.operation = application_operation::validate,
                           .code = output::error::revoked}});
    }
    if (!empty(desired)) {
        const auto valid =
            output::policy_detail::validate_internal_submit_request(request_for(desired),
                output::ttl_permission::finite_or_indefinite);
        if (!valid.has_value()) {
            return finish(
                {.failure = application_failure{.operation = application_operation::validate,
                     .code = valid.error()}});
        }
    }
    if (desired == desired_) {
        return finish();
    }

    desired_ = desired;
    replacement_pending_ = true;
    return maintain();
}

application_report application::maintain() {
    application_report report;
    if (closed_) {
        drain_all(report);
        return finish(report);
    }
    if (replacement_pending_) {
        drain_all(report);
        if (finish().pending_contexts != 0U || closed_) {
            return finish(report);
        }
        replacement_pending_ = false;
    }
    if (empty(desired_)) {
        return finish(report);
    }

    const auto assignments = presentation_.positions();
    for (std::size_t index = 0U; index < positions_.size(); ++index) {
        reconcile(positions_[index], assignments[index], report);
        if (closed_) {
            drain_all(report);
            break;
        }
    }
    return finish(report);
}

void application::reconcile(position& entry,
    const std::optional<xinput::assignment>& desired,
    application_report& report) {
    if (entry.assignment != desired) {
        if (!drain(entry, report) || closed_) {
            return;
        }
        entry.assignment = desired;
    }
    if (!entry.assignment.has_value() || !retire(entry.retiring, *entry.assignment, report) ||
        closed_) {
        return;
    }
    if (entry.current.has_value() && !entry.current->published) {
        publish(entry, report);
        return;
    }

    const auto opened = producer_.open(entry.assignment->target);
    if (!opened.has_value()) {
        record_failure(report, application_operation::open, opened.error(), *entry.assignment);
        if (opened.error() != output::error::resources) {
            static_cast<void>(retire(entry.current, *entry.assignment, report));
        }
        return;
    }
    if (entry.current.has_value() && entry.current->context == *opened) {
        return;
    }

    // Retain both identities before cleanup: a new foreground epoch can revoke the old one.
    entry.retiring =
        std::exchange(entry.current, obligation{.context = *opened, .receipt = std::nullopt});
    if (retire(entry.retiring, *entry.assignment, report) && !closed_) {
        publish(entry, report);
    }
}

void application::publish(position& entry, application_report& report) {
    if (!entry.current.has_value() || !entry.assignment.has_value()) {
        throw std::logic_error{"profile publication requires an owned context and assignment"};
    }
    auto& current = entry.current.value();
    const auto& assignment = entry.assignment.value();
    const auto submitted = producer_.submit(current.context, request_for(desired_));
    if (submitted.has_value()) {
        current.published = true;
        return;
    }
    record_failure(report,
        application_operation::submit,
        submitted.error(),
        assignment,
        current.context);
    if (submitted.error() != output::error::resources &&
        submitted.error() != output::error::capacity_exceeded) {
        static_cast<void>(retire(entry.current, assignment, report));
    }
}

} // namespace inputnext::profiles
