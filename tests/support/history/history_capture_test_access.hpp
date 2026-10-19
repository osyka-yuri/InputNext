#pragma once

#include <gameinput/history/history_capture.hpp>
#include <gameinput/history/history_capture_log.hpp>

#include <cstdint>
#include <mutex>

namespace inputnext::gameinput_detail {

struct capture_log_test_access final {
    static void set_next_position(history_capture_log& log, std::uint64_t position) noexcept {
        log.next_position_ = position;
    }
};

struct history_capture_test_access final {
    static void exhaust_log(history_capture& capture) noexcept {
        std::scoped_lock lock{capture.mutex_};
        capture.log_.exhaust();
    }

    static void set_next_position(history_capture& capture, std::uint64_t position) noexcept {
        std::scoped_lock lock{capture.mutex_};
        capture_log_test_access::set_next_position(capture.log_, position);
    }
};

} // namespace inputnext::gameinput_detail
