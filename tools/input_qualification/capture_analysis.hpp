#pragma once

#include "capture_report.hpp"

#include <cstdint>
#include <string_view>

namespace inputnext::input_qualification::diagnostic {

struct callback_capture_analysis final {
    std::uint64_t identity_failures{};
    std::uint64_t timestamp_regressions{};
    std::uint64_t timestamp_collisions{};
};

struct capture_overlap_analysis final {
    std::uint64_t identity_pairs{};
    std::uint64_t native_records_seen_in_callback{};
    std::uint64_t payload_matches{};
    std::uint64_t payload_mismatches{};
    std::uint64_t gesture_bearing_identity_matches{};
    std::uint64_t gesture_bearing_payload_matches{};
    std::uint64_t raw_touch_identity_pairs{};
    std::uint64_t raw_touch_payload_matches{};
    std::uint64_t raw_touch_payload_mismatches{};
};

struct raw_touch_capture_analysis final {
    std::uint64_t reads_attempted{};
    std::uint64_t reads_available{};
    std::uint64_t reads_missing{};
    std::uint64_t click_pressed{};
    std::uint64_t frames_with_contacts{};
    std::uint64_t contacts{};
};

struct capture_analysis final {
    callback_capture_analysis callbacks{};
    bool callback_unlabeled_activity{};
    bool native_unlabeled_activity{};
    capture_overlap_analysis forward_overlap{};
    capture_overlap_analysis tail_overlap{};
    raw_touch_capture_analysis callback_raw_touch{};
    raw_touch_capture_analysis native_raw_touch{};
    std::string_view qualification;
};

[[nodiscard]] capture_analysis analyze_capture(const report_context& report);

} // namespace inputnext::input_qualification::diagnostic
