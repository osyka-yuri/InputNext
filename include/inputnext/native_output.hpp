#pragma once

#include <inputnext/output_execution.hpp>

#include <inputnext/output.hpp>

namespace inputnext::output {

// Values supplied to a sink for its represented channels. Absent represented intents are neutral;
// OFF remains a present logical intent in the publication.
struct native_output final {
    channel_mask channels{};
    channel_values values{};

    friend constexpr bool operator==(const native_output&, const native_output&) noexcept = default;
};

struct native_command final {
    backend_handle backend{};
    publication_id publication{};
    attempt_id attempt{};
    native_output output{};
};

// The command is borrowed only for the duration of write. Implementations finish all native work
// before returning and do not retain its memory or defer an unqualified operation.
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
