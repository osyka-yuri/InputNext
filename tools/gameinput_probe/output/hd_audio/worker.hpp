#pragma once

#include "gameinput_probe/options.hpp"

#include <iosfwd>

namespace inputnext::gameinput_detail {

[[nodiscard]] int run_hd_audio_probe_worker(const probe_options& options, std::ostream& output);

} // namespace inputnext::gameinput_detail
