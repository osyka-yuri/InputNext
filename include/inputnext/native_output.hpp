#pragma once

#include <inputnext/output_execution.hpp>

namespace inputnext::output {

struct native_command final {
    backend_handle backend{};
    publication_id publication{};
    attempt_id attempt{};
    native_output output{};
};

// The command is borrowed only for the duration of write. Implementations finish all native work
// before returning and do not retain its memory or defer an unqualified operation. A sink
// registered with HD route support must preserve an accepted exact HD body route while later
// commands carry the same intent, including writes to other channels. If it cannot, it must return
// a nonaccepted result. This promise covers setter-route continuity, not physical playback.
class native_sink {
public:
    native_sink() = default;
    virtual ~native_sink() noexcept = default;

    native_sink(const native_sink&) = delete;
    native_sink& operator=(const native_sink&) = delete;
    native_sink(native_sink&&) = delete;
    native_sink& operator=(native_sink&&) = delete;

    [[nodiscard]] virtual native_result write(const native_command& command) noexcept = 0;
};

} // namespace inputnext::output
