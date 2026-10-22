#pragma once

#include "worker.hpp"

#include <iosfwd>

namespace inputnext::gameinput_detail {

[[nodiscard]] int run_rumble_probe_worker(const probe_options& options, std::ostream& output);

} // namespace inputnext::gameinput_detail
