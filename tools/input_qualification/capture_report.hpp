#pragma once

#include "report.hpp"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <span>
#include <string_view>

namespace inputnext::input_qualification::diagnostic {

inline constexpr std::uint32_t max_callback_records = 8192U;
inline constexpr std::uint32_t max_native_capture_records = 8192U;
inline constexpr std::uint32_t max_native_tail_records = 256U;

struct device_report;

struct native_order_diagnostics final {
    std::uint64_t identity_failures{};
    std::uint64_t same_timestamp_distinct_readings{};
    std::uint64_t timestamp_order_violations{};
};

struct reading_view final {
    std::reference_wrapper<const reading_sample> data;
    std::uintptr_t canonical_identity{};
};

struct native_capture_view final {
    std::span<const reading_view> frames;
    bool truncated{};
    std::string_view stop_reason;
    std::int32_t seed_hresult{};
    std::int32_t last_hresult{};
    std::uint64_t no_next_polls{};
    std::uint64_t transitions{};
    native_order_diagnostics order{};
    bool forward_complete{};
};

struct callback_capture_view final {
    std::span<const reading_view> records;
    std::uint64_t drops{};
    std::uint64_t missing_gamepad{};
    std::uint64_t short_button_state{};
    std::uint64_t truncated_contacts{};
};

struct report_context final {
    std::uint32_t capture_seconds{};
    std::string_view sdk_version;
    std::string_view runtime_file_version;
    std::string_view runtime_path;
    std::string_view architecture;
    std::uint64_t registration_errors{};
    std::uint64_t device_event_drops{};
    bool raw_usb_requested{};
    const device_report* device{};
    callback_capture_view callbacks{};
    native_capture_view native_forward{};
    native_capture_view native_tail{};
    bootstrap_measurement_summary bootstrap{};
};

void write_capture_report(std::ostream& output, const report_context& report);

} // namespace inputnext::input_qualification::diagnostic
