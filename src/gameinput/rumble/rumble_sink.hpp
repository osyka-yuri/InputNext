#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include "gameinput/runtime/output_access.hpp"
#include <inputnext/native_output.hpp>

#include <memory>

namespace inputnext::gameinput_detail {

[[nodiscard]] native_result<std::unique_ptr<output::native_sink>> make_rumble_sink(
    runtime_output_lease lease);

} // namespace inputnext::gameinput_detail
