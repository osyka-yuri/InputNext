#include "xinput_event_observer.hpp"

#include "xinput_event_report.hpp"
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <ostream>

namespace inputnext::gameinput_detail {
namespace {

constexpr std::size_t maximum_keystrokes_per_position = 64U;
constexpr auto guide_wait_retry_interval = std::chrono::seconds{1};

void drain_assignment_keystrokes(xinput::service& service,
    const xinput::assignment& assignment,
    bool report,
    std::ostream& output) {
    std::size_t drained{};
    DWORD last_result = ERROR_EMPTY;
    bool reached_empty{};
    while (drained < maximum_keystrokes_per_position) {
        XINPUT_KEYSTROKE keystroke{};
        last_result =
            service.get_keystroke(xinput::flavor::v1_4, assignment.user_index, 0U, &keystroke);
        if (last_result == ERROR_EMPTY) {
            reached_empty = true;
            break;
        }
        if (last_result != ERROR_SUCCESS) {
            if (report) {
                output << format_xinput_keystroke(assignment, last_result, nullptr) << '\n';
            }
            break;
        }
        output << format_xinput_keystroke(assignment, last_result, &keystroke) << '\n';
        ++drained;
    }

    if ((report && (reached_empty || last_result != ERROR_SUCCESS)) ||
        drained == maximum_keystrokes_per_position) {
        output << format_xinput_keystroke_drain({.user_index = assignment.user_index,
                      .result = reached_empty ? ERROR_EMPTY : last_result,
                      .drained = drained,
                      .limit_reached =
                          !reached_empty && drained == maximum_keystrokes_per_position})
               << '\n';
    }
}

} // namespace

xinput_event_observer::xinput_event_observer(gameinput_xinput& presentation) noexcept
    : presentation_(presentation) {
}

xinput_event_observer::~xinput_event_observer() {
    try {
        stop();
    } catch (...) {
        if (waiter_.joinable()) {
            presentation_.close();
            waiter_.join();
        }
    }
}

void xinput_event_observer::observe(maintenance_time now, bool report, std::ostream& output) {
    if (stopped_) {
        return;
    }
    collect_completed_wait(output);
    drain_keystrokes(report, output);
    if (report) {
        maybe_begin_guide_wait(now, output);
    }
}

void xinput_event_observer::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;

    auto& service = presentation_.presentation();
    if (active_ticket_.has_value() && waiter_.joinable() &&
        !waiter_done_.load(std::memory_order_acquire)) {
        try {
            pending_cancel_report_ = guide_cancel_report{
                .ticket = *active_ticket_,
                .result = service.cancel_guide_wait(*active_ticket_),
            };
        } catch (...) {
            pending_cancel_report_.reset();
        }
    }

    presentation_.close();
    finish_wait();
}

void xinput_event_observer::report_shutdown(std::ostream& output) {
    report_pending(output);
}

void xinput_event_observer::drain_keystrokes(bool report, std::ostream& output) {
    auto& service = presentation_.presentation();
    const auto positions = service.positions();
    bool assigned{};
    for (const auto& assignment : positions) {
        const auto selected_assignment = assignment.value_or(xinput::assignment{});
        if (!assignment) {
            continue;
        }
        assigned = true;
        drain_assignment_keystrokes(service, selected_assignment, report, output);
    }
    if (!assigned && report) {
        XINPUT_KEYSTROKE keystroke{};
        const DWORD result = service.get_keystroke(xinput::flavor::v1_4, 0U, 0U, &keystroke);
        output << format_xinput_keystroke_drain(
                      {.user_index = 0U, .result = result, .drained = 0U, .limit_reached = false})
               << '\n';
    }
}

void xinput_event_observer::collect_completed_wait(std::ostream& output) {
    if (!active_ticket_.has_value() || !waiter_.joinable() ||
        !waiter_done_.load(std::memory_order_acquire)) {
        return;
    }

    finish_wait();
    report_pending(output);
}

void xinput_event_observer::finish_wait() {
    if (!active_ticket_.has_value()) {
        return;
    }
    if (waiter_.joinable()) {
        waiter_.join();
    }

    const auto ticket = *active_ticket_;
    if (!waiter_result_.has_value()) {
        bool released{};
        try {
            released = presentation_.presentation().release_guide_wait(ticket).has_value();
        } catch (...) {
            released = false;
        }
        pending_wait_report_ = guide_wait_report{
            .ticket = ticket,
            .result = std::nullopt,
            .ticket_released = released,
        };
    } else {
        pending_wait_report_ = guide_wait_report{
            .ticket = ticket,
            .result = waiter_result_,
            .ticket_released = false,
        };
    }
    active_ticket_.reset();
}

void xinput_event_observer::report_pending(std::ostream& output) {
    if (pending_cancel_report_.has_value()) {
        const auto report = pending_cancel_report_.value();
        output << format_xinput_guide_cancel(report.ticket, report.result) << '\n';
        pending_cancel_report_.reset();
    }
    if (pending_wait_report_.has_value()) {
        const auto report = pending_wait_report_.value();
        if (report.result.has_value()) {
            output << format_xinput_guide_wait(report.ticket, *report.result) << '\n';
        } else {
            output << format_xinput_guide_wait_exception(report.ticket, report.ticket_released)
                   << '\n';
        }
        pending_wait_report_.reset();
    }
}

void xinput_event_observer::maybe_begin_guide_wait(maintenance_time now, std::ostream& output) {
    if (guide_wait_admitted_ || active_ticket_.has_value() || now < next_begin_attempt_) {
        return;
    }

    const auto positions = presentation_.presentation().positions();
    const auto occupied = std::ranges::find_if(positions,
        [](const auto& assignment) { return assignment.has_value(); });
    const DWORD user_index =
        occupied == positions.end() ? 0U : occupied->value_or(xinput::assignment{}).user_index;
    auto begun = presentation_.presentation().begin_guide_wait(user_index);
    output << format_xinput_guide_begin(user_index, begun) << '\n';
    next_begin_attempt_ = now + guide_wait_retry_interval;
    if (!begun.has_value()) {
        return;
    }

    const auto ticket = *begun;
    waiter_done_.store(false, std::memory_order_release);
    waiter_result_.reset();
    active_ticket_ = ticket;
    try {
        waiter_ = std::thread([this, ticket] {
            try {
                waiter_result_ = presentation_.presentation().wait_for_guide_button(ticket);
            } catch (...) {
                waiter_result_.reset();
            }
            waiter_done_.store(true, std::memory_order_release);
        });
        guide_wait_admitted_ = true;
    } catch (...) {
        bool released{};
        try {
            released = presentation_.presentation().release_guide_wait(ticket).has_value();
        } catch (...) {
            released = false;
        }
        active_ticket_.reset();
        waiter_done_.store(true, std::memory_order_release);
        output << format_xinput_guide_wait_start_failure(ticket, released) << '\n';
    }
}

} // namespace inputnext::gameinput_detail
