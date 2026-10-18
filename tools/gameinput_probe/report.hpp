#pragma once

#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <string>

#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/input_binding.hpp>

namespace inputnext::gameinput_detail {

struct probe_requirements final {
    bool device_required{};
    bool reading_required{};
    bool device_observed{};
    bool reading_observed{};
};

struct requirement_report final {
    bool met{};
    std::string line;
};

[[nodiscard]] std::string format_device(const gameinput_connection& connection,
    const result<connection_snapshot>& sample);
[[nodiscard]] std::string format_metadata(const metadata_snapshot& snapshot);
[[nodiscard]] std::string format_metadata_result(connection_target target,
    const metadata_result<metadata_snapshot>& metadata);
[[nodiscard]] std::string format_start_failure(gameinput_failure failure);
[[nodiscard]] std::string format_stop_report(gameinput_stop_report report);
[[nodiscard]] requirement_report format_requirements(probe_requirements requirements);

} // namespace inputnext::gameinput_detail
