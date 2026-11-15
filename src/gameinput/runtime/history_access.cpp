#include "history_access.hpp"

#include "history/reader_observation.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/history.hpp>

#include <cstddef>

namespace inputnext::gameinput_detail {

history_result<history_reader_handle> runtime_history_access::open_directinput(
    gameinput_runtime& runtime,
    connection_target target,
    std::size_t lane_index,
    history_time now) {
    return runtime.implementation_->open_directinput_history(target, lane_index, now);
}

history_detail::observed_read runtime_history_access::read_directinput(gameinput_runtime& runtime,
    history_reader_handle reader,
    history_time now,
    bool capture_boundary) {
    return runtime.implementation_->read_directinput_history(reader, now, capture_boundary);
}

} // namespace inputnext::gameinput_detail
