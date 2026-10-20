#pragma once

#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/gameinput_xinput.hpp>
#include <inputnext/xinput_events.hpp>

#include <atomic>
#include <optional>
#include <ostream>
#include <thread>

namespace inputnext::gameinput_detail {

class xinput_event_observer final {
public:
    explicit xinput_event_observer(gameinput_xinput& presentation) noexcept;
    ~xinput_event_observer();

    xinput_event_observer(const xinput_event_observer&) = delete;
    xinput_event_observer& operator=(const xinput_event_observer&) = delete;
    xinput_event_observer(xinput_event_observer&&) = delete;
    xinput_event_observer& operator=(xinput_event_observer&&) = delete;

    void observe(maintenance_time now, bool report, std::ostream& output);
    void stop();
    void report_shutdown(std::ostream& output);

private:
    struct guide_wait_report final {
        xinput::guide_wait_ticket ticket{};
        std::optional<xinput::guide_wait_result<void>> result;
        bool ticket_released{};
    };

    struct guide_cancel_report final {
        xinput::guide_wait_ticket ticket{};
        xinput::guide_wait_result<bool> result;
    };

    void drain_keystrokes(bool report, std::ostream& output);
    void collect_completed_wait(std::ostream& output);
    void finish_wait();
    void report_pending(std::ostream& output);
    void maybe_begin_guide_wait(maintenance_time now, std::ostream& output);

    gameinput_xinput& presentation_;
    std::thread waiter_;
    std::atomic<bool> waiter_done_{true};
    std::optional<xinput::guide_wait_result<void>> waiter_result_;
    std::optional<xinput::guide_wait_ticket> active_ticket_;
    std::optional<guide_wait_report> pending_wait_report_;
    std::optional<guide_cancel_report> pending_cancel_report_;
    maintenance_time next_begin_attempt_{maintenance_time::min()};
    bool guide_wait_admitted_{};
    bool stopped_{};
};

} // namespace inputnext::gameinput_detail
