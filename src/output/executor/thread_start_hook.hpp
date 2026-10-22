#pragma once

#ifdef INPUTNEXT_OUTPUT_TESTING
#    include <cstdint>

namespace inputnext::output::detail::thread_start_testing {

enum class role : std::uint8_t { none, coordinator, delivery_worker };

void before_start(role value);

} // namespace inputnext::output::detail::thread_start_testing
#endif
