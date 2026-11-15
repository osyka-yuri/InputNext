#pragma once

#include "history/reader_observation.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <cstddef>

namespace inputnext::gameinput_detail {

class runtime_history_access final {
public:
    [[nodiscard]] static history_result<history_reader_handle> open_directinput(
        gameinput_runtime& runtime,
        connection_target target,
        std::size_t lane_index,
        history_time now);
    [[nodiscard]] static history_detail::observed_read read_directinput(gameinput_runtime& runtime,
        history_reader_handle reader,
        history_time now,
        bool capture_boundary);
};

} // namespace inputnext::gameinput_detail
